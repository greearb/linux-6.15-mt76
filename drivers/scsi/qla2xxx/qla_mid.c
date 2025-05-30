// SPDX-License-Identifier: GPL-2.0-only
/*
 * QLogic Fibre Channel HBA Driver
 * Copyright (c)  2003-2014 QLogic Corporation
 */
#include "qla_def.h"
#include "qla_gbl.h"
#include "qla_target.h"

#include <linux/moduleparam.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/list.h>

#include <scsi/scsi_tcq.h>
#include <scsi/scsicam.h>
#include <linux/delay.h>

void
qla2x00_vp_stop_timer(scsi_qla_host_t *vha)
{
	if (vha->vp_idx && vha->timer_active) {
		timer_delete_sync(&vha->timer);
		vha->timer_active = 0;
	}
}

static uint32_t
qla24xx_allocate_vp_id(scsi_qla_host_t *vha)
{
	uint32_t vp_id;
	struct qla_hw_data *ha = vha->hw;
	unsigned long flags;

	/* Find an empty slot and assign an vp_id */
	mutex_lock(&ha->vport_lock);
	vp_id = find_first_zero_bit(ha->vp_idx_map, ha->max_npiv_vports + 1);
	if (vp_id > ha->max_npiv_vports) {
		ql_dbg(ql_dbg_vport, vha, 0xa000,
		    "vp_id %d is bigger than max-supported %d.\n",
		    vp_id, ha->max_npiv_vports);
		mutex_unlock(&ha->vport_lock);
		return vp_id;
	}

	set_bit(vp_id, ha->vp_idx_map);
	ha->num_vhosts++;
	vha->vp_idx = vp_id;

	spin_lock_irqsave(&ha->vport_slock, flags);
	list_add_tail(&vha->list, &ha->vp_list);
	spin_unlock_irqrestore(&ha->vport_slock, flags);

	spin_lock_irqsave(&ha->hardware_lock, flags);
	qla_update_vp_map(vha, SET_VP_IDX);
	spin_unlock_irqrestore(&ha->hardware_lock, flags);

	mutex_unlock(&ha->vport_lock);
	return vp_id;
}

void
qla24xx_deallocate_vp_id(scsi_qla_host_t *vha)
{
	uint16_t vp_id;
	struct qla_hw_data *ha = vha->hw;
	unsigned long flags = 0;
	u32 i, bailout;

	mutex_lock(&ha->vport_lock);
	/*
	 * Wait for all pending activities to finish before removing vport from
	 * the list.
	 * Lock needs to be held for safe removal from the list (it
	 * ensures no active vp_list traversal while the vport is removed
	 * from the queue)
	 */
	bailout = 0;
	for (i = 0; i < 500; i++) {
		spin_lock_irqsave(&ha->vport_slock, flags);
		if (atomic_read(&vha->vref_count) == 0) {
			list_del(&vha->list);
			qla_update_vp_map(vha, RESET_VP_IDX);
			bailout = 1;
		}
		spin_unlock_irqrestore(&ha->vport_slock, flags);

		if (bailout)
			break;
		else
			msleep(20);
	}
	if (!bailout) {
		ql_log(ql_log_info, vha, 0xfffa,
			"vha->vref_count=%u timeout\n", vha->vref_count.counter);
		spin_lock_irqsave(&ha->vport_slock, flags);
		list_del(&vha->list);
		qla_update_vp_map(vha, RESET_VP_IDX);
		spin_unlock_irqrestore(&ha->vport_slock, flags);
	}

	vp_id = vha->vp_idx;
	ha->num_vhosts--;
	clear_bit(vp_id, ha->vp_idx_map);

	mutex_unlock(&ha->vport_lock);
}

static scsi_qla_host_t *
qla24xx_find_vhost_by_name(struct qla_hw_data *ha, uint8_t *port_name)
{
	scsi_qla_host_t *vha;
	struct scsi_qla_host *tvha;
	unsigned long flags;

	spin_lock_irqsave(&ha->vport_slock, flags);
	/* Locate matching device in database. */
	list_for_each_entry_safe(vha, tvha, &ha->vp_list, list) {
		if (!memcmp(port_name, vha->port_name, WWN_SIZE)) {
			spin_unlock_irqrestore(&ha->vport_slock, flags);
			return vha;
		}
	}
	spin_unlock_irqrestore(&ha->vport_slock, flags);
	return NULL;
}

/*
 * qla2x00_mark_vp_devices_dead
 *	Updates fcport state when device goes offline.
 *
 * Input:
 *	ha = adapter block pointer.
 *	fcport = port structure pointer.
 *
 * Return:
 *	None.
 *
 * Context:
 */
static void
qla2x00_mark_vp_devices_dead(scsi_qla_host_t *vha)
{
	/*
	 * !!! NOTE !!!
	 * This function, if called in contexts other than vp create, disable
	 * or delete, please make sure this is synchronized with the
	 * delete thread.
	 */
	fc_port_t *fcport;

	list_for_each_entry(fcport, &vha->vp_fcports, list) {
		ql_dbg(ql_dbg_vport, vha, 0xa001,
		    "Marking port dead, loop_id=0x%04x : %x.\n",
		    fcport->loop_id, fcport->vha->vp_idx);

		qla2x00_mark_device_lost(vha, fcport, 0);
		qla2x00_set_fcport_state(fcport, FCS_UNCONFIGURED);
	}
}

int
qla24xx_disable_vp(scsi_qla_host_t *vha)
{
	unsigned long flags;
	int ret = QLA_SUCCESS;
	fc_port_t *fcport;

	if (vha->hw->flags.edif_enabled) {
		if (DBELL_ACTIVE(vha))
			qla2x00_post_aen_work(vha, FCH_EVT_VENDOR_UNIQUE,
			    FCH_EVT_VENDOR_UNIQUE_VPORT_DOWN);
		/* delete sessions and flush sa_indexes */
		qla2x00_wait_for_sess_deletion(vha);
	}

	if (vha->hw->flags.fw_started)
		ret = qla24xx_control_vp(vha, VCE_COMMAND_DISABLE_VPS_LOGO_ALL);

	atomic_set(&vha->loop_state, LOOP_DOWN);
	atomic_set(&vha->loop_down_timer, LOOP_DOWN_TIME);
	list_for_each_entry(fcport, &vha->vp_fcports, list)
		fcport->logout_on_delete = 1;

	if (!vha->hw->flags.edif_enabled)
		qla2x00_wait_for_sess_deletion(vha);

	/* Remove port id from vp target map */
	spin_lock_irqsave(&vha->hw->hardware_lock, flags);
	qla_update_vp_map(vha, RESET_AL_PA);
	spin_unlock_irqrestore(&vha->hw->hardware_lock, flags);

	qla2x00_mark_vp_devices_dead(vha);
	atomic_set(&vha->vp_state, VP_FAILED);
	vha->flags.management_server_logged_in = 0;
	if (ret == QLA_SUCCESS) {
		fc_vport_set_state(vha->fc_vport, FC_VPORT_DISABLED);
	} else {
		fc_vport_set_state(vha->fc_vport, FC_VPORT_FAILED);
		return -1;
	}
	return 0;
}

int
qla24xx_enable_vp(scsi_qla_host_t *vha)
{
	int ret;
	struct qla_hw_data *ha = vha->hw;
	scsi_qla_host_t *base_vha = pci_get_drvdata(ha->pdev);

	/* Check if physical ha port is Up */
	if (atomic_read(&base_vha->loop_state) == LOOP_DOWN  ||
		atomic_read(&base_vha->loop_state) == LOOP_DEAD ||
		!(ha->current_topology & ISP_CFG_F)) {
		vha->vp_err_state =  VP_ERR_PORTDWN;
		fc_vport_set_state(vha->fc_vport, FC_VPORT_LINKDOWN);
		ql_dbg(ql_dbg_taskm, vha, 0x800b,
		    "%s skip enable. loop_state %x topo %x\n",
		    __func__, base_vha->loop_state.counter,
		    ha->current_topology);

		goto enable_failed;
	}

	/* Initialize the new vport unless it is a persistent port */
	mutex_lock(&ha->vport_lock);
	ret = qla24xx_modify_vp_config(vha);
	mutex_unlock(&ha->vport_lock);

	if (ret != QLA_SUCCESS) {
		fc_vport_set_state(vha->fc_vport, FC_VPORT_FAILED);
		goto enable_failed;
	}

	ql_dbg(ql_dbg_taskm, vha, 0x801a,
	    "Virtual port with id: %d - Enabled.\n", vha->vp_idx);
	return 0;

enable_failed:
	ql_dbg(ql_dbg_taskm, vha, 0x801b,
	    "Virtual port with id: %d - Disabled.\n", vha->vp_idx);
	return 1;
}

static void
qla24xx_configure_vp(scsi_qla_host_t *vha)
{
	struct fc_vport *fc_vport;
	int ret;

	fc_vport = vha->fc_vport;

	ql_dbg(ql_dbg_vport, vha, 0xa002,
	    "%s: change request #3.\n", __func__);
	ret = qla2x00_send_change_request(vha, 0x3, vha->vp_idx);
	if (ret != QLA_SUCCESS) {
		ql_dbg(ql_dbg_vport, vha, 0xa003, "Failed to enable "
		    "receiving of RSCN requests: 0x%x.\n", ret);
		return;
	} else {
		/* Corresponds to SCR enabled */
		clear_bit(VP_SCR_NEEDED, &vha->vp_flags);
	}

	vha->flags.online = 1;
	if (qla24xx_configure_vhba(vha))
		return;

	atomic_set(&vha->vp_state, VP_ACTIVE);
	fc_vport_set_state(fc_vport, FC_VPORT_ACTIVE);
}

void
qla2x00_alert_all_vps(struct rsp_que *rsp, uint16_t *mb)
{
	scsi_qla_host_t *vha, *tvp;
	struct qla_hw_data *ha = rsp->hw;
	int i = 0;
	unsigned long flags;

	spin_lock_irqsave(&ha->vport_slock, flags);
	list_for_each_entry_safe(vha, tvp, &ha->vp_list, list) {
		if (vha->vp_idx) {
			if (test_bit(VPORT_DELETE, &vha->dpc_flags))
				continue;

			atomic_inc(&vha->vref_count);
			spin_unlock_irqrestore(&ha->vport_slock, flags);

			switch (mb[0]) {
			case MBA_LIP_OCCURRED:
			case MBA_LOOP_UP:
			case MBA_LOOP_DOWN:
			case MBA_LIP_RESET:
			case MBA_POINT_TO_POINT:
			case MBA_CHG_IN_CONNECTION:
				ql_dbg(ql_dbg_async, vha, 0x5024,
				    "Async_event for VP[%d], mb=0x%x vha=%p.\n",
				    i, *mb, vha);
				qla2x00_async_event(vha, rsp, mb);
				break;
			case MBA_PORT_UPDATE:
			case MBA_RSCN_UPDATE:
				if ((mb[3] & 0xff) == vha->vp_idx) {
					ql_dbg(ql_dbg_async, vha, 0x5024,
					    "Async_event for VP[%d], mb=0x%x vha=%p\n",
					    i, *mb, vha);
					qla2x00_async_event(vha, rsp, mb);
				}
				break;
			}

			spin_lock_irqsave(&ha->vport_slock, flags);
			atomic_dec(&vha->vref_count);
			wake_up(&vha->vref_waitq);
		}
		i++;
	}
	spin_unlock_irqrestore(&ha->vport_slock, flags);
}

int
qla2x00_vp_abort_isp(scsi_qla_host_t *vha)
{
	fc_port_t *fcport;

	/*
	 * To exclusively reset vport, we need to log it out first.
	 * Note: This control_vp can fail if ISP reset is already
	 * issued, this is expected, as the vp would be already
	 * logged out due to ISP reset.
	 */
	if (!test_bit(ABORT_ISP_ACTIVE, &vha->dpc_flags)) {
		qla24xx_control_vp(vha, VCE_COMMAND_DISABLE_VPS_LOGO_ALL);
		list_for_each_entry(fcport, &vha->vp_fcports, list)
			fcport->logout_on_delete = 0;
	}

	/*
	 * Physical port will do most of the abort and recovery work. We can
	 * just treat it as a loop down
	 */
	if (atomic_read(&vha->loop_state) != LOOP_DOWN) {
		atomic_set(&vha->loop_state, LOOP_DOWN);
		qla2x00_mark_all_devices_lost(vha);
	} else {
		if (!atomic_read(&vha->loop_down_timer))
			atomic_set(&vha->loop_down_timer, LOOP_DOWN_TIME);
	}

	ql_dbg(ql_dbg_taskm, vha, 0x801d,
	    "Scheduling enable of Vport %d.\n", vha->vp_idx);

	return qla24xx_enable_vp(vha);
}

static int
qla2x00_do_dpc_vp(scsi_qla_host_t *vha)
{
	struct qla_hw_data *ha = vha->hw;
	scsi_qla_host_t *base_vha = pci_get_drvdata(ha->pdev);

	ql_dbg(ql_dbg_dpc + ql_dbg_verbose, vha, 0x4012,
	    "Entering %s vp_flags: 0x%lx.\n", __func__, vha->vp_flags);

	/* Check if Fw is ready to configure VP first */
	if (test_bit(VP_CONFIG_OK, &base_vha->vp_flags)) {
		if (test_and_clear_bit(VP_IDX_ACQUIRED, &vha->vp_flags)) {
			/* VP acquired. complete port configuration */
			ql_dbg(ql_dbg_dpc, vha, 0x4014,
			    "Configure VP scheduled.\n");
			qla24xx_configure_vp(vha);
			ql_dbg(ql_dbg_dpc, vha, 0x4015,
			    "Configure VP end.\n");
			return 0;
		}
	}

	if (test_bit(PROCESS_PUREX_IOCB, &vha->dpc_flags)) {
		if (atomic_read(&vha->loop_state) == LOOP_READY) {
			qla24xx_process_purex_list(&vha->purex_list);
			clear_bit(PROCESS_PUREX_IOCB, &vha->dpc_flags);
		}
	}

	if (test_bit(RELOGIN_NEEDED, &vha->dpc_flags) &&
	    !test_bit(LOOP_RESYNC_NEEDED, &vha->dpc_flags) &&
	    atomic_read(&vha->loop_state) != LOOP_DOWN) {

		if (!vha->relogin_jif ||
		    time_after_eq(jiffies, vha->relogin_jif)) {
			vha->relogin_jif = jiffies + HZ;
			clear_bit(RELOGIN_NEEDED, &vha->dpc_flags);

			ql_dbg(ql_dbg_dpc, vha, 0x4018,
			    "Relogin needed scheduled.\n");
			qla24xx_post_relogin_work(vha);
		}
	}

	if (test_and_clear_bit(RESET_MARKER_NEEDED, &vha->dpc_flags) &&
	    (!(test_and_set_bit(RESET_ACTIVE, &vha->dpc_flags)))) {
		clear_bit(RESET_ACTIVE, &vha->dpc_flags);
	}

	if (test_and_clear_bit(LOOP_RESYNC_NEEDED, &vha->dpc_flags)) {
		if (!(test_and_set_bit(LOOP_RESYNC_ACTIVE, &vha->dpc_flags))) {
			ql_dbg(ql_dbg_dpc, vha, 0x401a,
			    "Loop resync scheduled.\n");
			qla2x00_loop_resync(vha);
			clear_bit(LOOP_RESYNC_ACTIVE, &vha->dpc_flags);
			ql_dbg(ql_dbg_dpc, vha, 0x401b,
			    "Loop resync end.\n");
		}
	}

	ql_dbg(ql_dbg_dpc + ql_dbg_verbose, vha, 0x401c,
	    "Exiting %s.\n", __func__);
	return 0;
}

void
qla2x00_do_dpc_all_vps(scsi_qla_host_t *vha)
{
	struct qla_hw_data *ha = vha->hw;
	scsi_qla_host_t *vp, *tvp;
	unsigned long flags = 0;

	if (vha->vp_idx)
		return;
	if (list_empty(&ha->vp_list))
		return;

	clear_bit(VP_DPC_NEEDED, &vha->dpc_flags);

	if (!(ha->current_topology & ISP_CFG_F))
		return;

	spin_lock_irqsave(&ha->vport_slock, flags);
	list_for_each_entry_safe(vp, tvp, &ha->vp_list, list) {
		if (vp->vp_idx) {
			atomic_inc(&vp->vref_count);
			spin_unlock_irqrestore(&ha->vport_slock, flags);

			qla2x00_do_dpc_vp(vp);

			spin_lock_irqsave(&ha->vport_slock, flags);
			atomic_dec(&vp->vref_count);
		}
	}
	spin_unlock_irqrestore(&ha->vport_slock, flags);
}

int
qla24xx_vport_create_req_sanity_check(struct fc_vport *fc_vport)
{
	scsi_qla_host_t *base_vha = shost_priv(fc_vport->shost);
	struct qla_hw_data *ha = base_vha->hw;
	scsi_qla_host_t *vha;
	uint8_t port_name[WWN_SIZE];

	if (fc_vport->roles != FC_PORT_ROLE_FCP_INITIATOR)
		return VPCERR_UNSUPPORTED;

	/* Check up the F/W and H/W support NPIV */
	if (!ha->flags.npiv_supported)
		return VPCERR_UNSUPPORTED;

	/* Check up whether npiv supported switch presented */
	if (!(ha->switch_cap & FLOGI_MID_SUPPORT))
		return VPCERR_NO_FABRIC_SUPP;

	/* Check up unique WWPN */
	u64_to_wwn(fc_vport->port_name, port_name);
	if (!memcmp(port_name, base_vha->port_name, WWN_SIZE))
		return VPCERR_BAD_WWN;
	vha = qla24xx_find_vhost_by_name(ha, port_name);
	if (vha)
		return VPCERR_BAD_WWN;

	/* Check up max-npiv-supports */
	if (ha->num_vhosts > ha->max_npiv_vports) {
		ql_dbg(ql_dbg_vport, vha, 0xa004,
		    "num_vhosts %ud is bigger "
		    "than max_npiv_vports %ud.\n",
		    ha->num_vhosts, ha->max_npiv_vports);
		return VPCERR_UNSUPPORTED;
	}
	return 0;
}

scsi_qla_host_t *
qla24xx_create_vhost(struct fc_vport *fc_vport)
{
	scsi_qla_host_t *base_vha = shost_priv(fc_vport->shost);
	struct qla_hw_data *ha = base_vha->hw;
	scsi_qla_host_t *vha;
	const struct scsi_host_template *sht = &qla2xxx_driver_template;
	struct Scsi_Host *host;

	vha = qla2x00_create_host(sht, ha);
	if (!vha) {
		ql_log(ql_log_warn, vha, 0xa005,
		    "scsi_host_alloc() failed for vport.\n");
		return(NULL);
	}

	vha->irq_offset = QLA_BASE_VECTORS;
	host = vha->host;
	fc_vport->dd_data = vha;
	/* New host info */
	u64_to_wwn(fc_vport->node_name, vha->node_name);
	u64_to_wwn(fc_vport->port_name, vha->port_name);

	vha->fc_vport = fc_vport;
	vha->device_flags = 0;
	vha->vp_idx = qla24xx_allocate_vp_id(vha);
	if (vha->vp_idx > ha->max_npiv_vports) {
		ql_dbg(ql_dbg_vport, vha, 0xa006,
		    "Couldn't allocate vp_id.\n");
		goto create_vhost_failed;
	}
	vha->mgmt_svr_loop_id = qla2x00_reserve_mgmt_server_loop_id(vha);

	vha->dpc_flags = 0L;
	ha->dpc_active = 0;
	set_bit(REGISTER_FDMI_NEEDED, &vha->dpc_flags);
	set_bit(REGISTER_FC4_NEEDED, &vha->dpc_flags);

	/*
	 * To fix the issue of processing a parent's RSCN for the vport before
	 * its SCR is complete.
	 */
	set_bit(VP_SCR_NEEDED, &vha->vp_flags);
	atomic_set(&vha->loop_state, LOOP_DOWN);
	atomic_set(&vha->loop_down_timer, LOOP_DOWN_TIME);

	qla2x00_start_timer(vha, WATCH_INTERVAL);

	vha->req = base_vha->req;
	vha->flags.nvme_enabled = base_vha->flags.nvme_enabled;
	host->can_queue = base_vha->req->length + 128;
	host->cmd_per_lun = 3;
	if (IS_T10_PI_CAPABLE(ha) && ql2xenabledif)
		host->max_cmd_len = 32;
	else
		host->max_cmd_len = MAX_CMDSZ;
	host->max_channel = MAX_BUSES - 1;
	host->max_lun = ql2xmaxlun;
	host->unique_id = host->host_no;
	host->max_id = ha->max_fibre_devices;
	host->transportt = qla2xxx_transport_vport_template;

	ql_dbg(ql_dbg_vport, vha, 0xa007,
	    "Detect vport hba %ld at address = %p.\n",
	    vha->host_no, vha);

	vha->flags.init_done = 1;

	mutex_lock(&ha->vport_lock);
	set_bit(vha->vp_idx, ha->vp_idx_map);
	ha->cur_vport_count++;
	mutex_unlock(&ha->vport_lock);

	return vha;

create_vhost_failed:
	return NULL;
}

static void
qla25xx_free_req_que(struct scsi_qla_host *vha, struct req_que *req)
{
	struct qla_hw_data *ha = vha->hw;
	uint16_t que_id = req->id;

	dma_free_coherent(&ha->pdev->dev, (req->length + 1) *
		sizeof(request_t), req->ring, req->dma);
	req->ring = NULL;
	req->dma = 0;
	if (que_id) {
		ha->req_q_map[que_id] = NULL;
		mutex_lock(&ha->vport_lock);
		clear_bit(que_id, ha->req_qid_map);
		mutex_unlock(&ha->vport_lock);
	}
	kfree(req->outstanding_cmds);
	kfree(req);
}

static void
qla25xx_free_rsp_que(struct scsi_qla_host *vha, struct rsp_que *rsp)
{
	struct qla_hw_data *ha = vha->hw;
	uint16_t que_id = rsp->id;

	if (rsp->msix && rsp->msix->have_irq) {
		free_irq(rsp->msix->vector, rsp->msix->handle);
		rsp->msix->have_irq = 0;
		rsp->msix->in_use = 0;
		rsp->msix->handle = NULL;
	}
	dma_free_coherent(&ha->pdev->dev, (rsp->length + 1) *
		sizeof(response_t), rsp->ring, rsp->dma);
	rsp->ring = NULL;
	rsp->dma = 0;
	if (que_id) {
		ha->rsp_q_map[que_id] = NULL;
		mutex_lock(&ha->vport_lock);
		clear_bit(que_id, ha->rsp_qid_map);
		mutex_unlock(&ha->vport_lock);
	}
	kfree(rsp);
}

int
qla25xx_delete_req_que(struct scsi_qla_host *vha, struct req_que *req)
{
	int ret = QLA_SUCCESS;

	if (req && vha->flags.qpairs_req_created) {
		req->options |= BIT_0;
		ret = qla25xx_init_req_que(vha, req);
		if (ret != QLA_SUCCESS)
			return QLA_FUNCTION_FAILED;

		qla25xx_free_req_que(vha, req);
	}

	return ret;
}

int
qla25xx_delete_rsp_que(struct scsi_qla_host *vha, struct rsp_que *rsp)
{
	int ret = QLA_SUCCESS;

	if (rsp && vha->flags.qpairs_rsp_created) {
		rsp->options |= BIT_0;
		ret = qla25xx_init_rsp_que(vha, rsp);
		if (ret != QLA_SUCCESS)
			return QLA_FUNCTION_FAILED;

		qla25xx_free_rsp_que(vha, rsp);
	}

	return ret;
}

/* Delete all queues for a given vhost */
int
qla25xx_delete_queues(struct scsi_qla_host *vha)
{
	int cnt, ret = 0;
	struct req_que *req = NULL;
	struct rsp_que *rsp = NULL;
	struct qla_hw_data *ha = vha->hw;
	struct qla_qpair *qpair, *tqpair;

	if (ql2xmqsupport || ql2xnvmeenable) {
		list_for_each_entry_safe(qpair, tqpair, &vha->qp_list,
		    qp_list_elem)
			qla2xxx_delete_qpair(vha, qpair);
	} else {
		/* Delete request queues */
		for (cnt = 1; cnt < ha->max_req_queues; cnt++) {
			req = ha->req_q_map[cnt];
			if (req && test_bit(cnt, ha->req_qid_map)) {
				ret = qla25xx_delete_req_que(vha, req);
				if (ret != QLA_SUCCESS) {
					ql_log(ql_log_warn, vha, 0x00ea,
					    "Couldn't delete req que %d.\n",
					    req->id);
					return ret;
				}
			}
		}

		/* Delete response queues */
		for (cnt = 1; cnt < ha->max_rsp_queues; cnt++) {
			rsp = ha->rsp_q_map[cnt];
			if (rsp && test_bit(cnt, ha->rsp_qid_map)) {
				ret = qla25xx_delete_rsp_que(vha, rsp);
				if (ret != QLA_SUCCESS) {
					ql_log(ql_log_warn, vha, 0x00eb,
					    "Couldn't delete rsp que %d.\n",
					    rsp->id);
					return ret;
				}
			}
		}
	}

	return ret;
}

int
qla25xx_create_req_que(struct qla_hw_data *ha, uint16_t options,
    uint8_t vp_idx, uint16_t rid, int rsp_que, uint8_t qos, bool startqp)
{
	int ret = 0;
	struct req_que *req = NULL;
	struct scsi_qla_host *base_vha = pci_get_drvdata(ha->pdev);
	struct scsi_qla_host *vha = pci_get_drvdata(ha->pdev);
	uint16_t que_id = 0;
	device_reg_t *reg;
	uint32_t cnt;

	req = kzalloc(sizeof(struct req_que), GFP_KERNEL);
	if (req == NULL) {
		ql_log(ql_log_fatal, base_vha, 0x00d9,
		    "Failed to allocate memory for request queue.\n");
		goto failed;
	}

	req->length = REQUEST_ENTRY_CNT_24XX;
	req->ring = dma_alloc_coherent(&ha->pdev->dev,
			(req->length + 1) * sizeof(request_t),
			&req->dma, GFP_KERNEL);
	if (req->ring == NULL) {
		ql_log(ql_log_fatal, base_vha, 0x00da,
		    "Failed to allocate memory for request_ring.\n");
		goto que_failed;
	}

	ret = qla2x00_alloc_outstanding_cmds(ha, req);
	if (ret != QLA_SUCCESS)
		goto que_failed;

	mutex_lock(&ha->mq_lock);
	que_id = find_first_zero_bit(ha->req_qid_map, ha->max_req_queues);
	if (que_id >= ha->max_req_queues) {
		mutex_unlock(&ha->mq_lock);
		ql_log(ql_log_warn, base_vha, 0x00db,
		    "No resources to create additional request queue.\n");
		goto que_failed;
	}
	set_bit(que_id, ha->req_qid_map);
	ha->req_q_map[que_id] = req;
	req->rid = rid;
	req->vp_idx = vp_idx;
	req->qos = qos;

	ql_dbg(ql_dbg_multiq, base_vha, 0xc002,
	    "queue_id=%d rid=%d vp_idx=%d qos=%d.\n",
	    que_id, req->rid, req->vp_idx, req->qos);
	ql_dbg(ql_dbg_init, base_vha, 0x00dc,
	    "queue_id=%d rid=%d vp_idx=%d qos=%d.\n",
	    que_id, req->rid, req->vp_idx, req->qos);
	if (rsp_que < 0)
		req->rsp = NULL;
	else
		req->rsp = ha->rsp_q_map[rsp_que];
	/* Use alternate PCI bus number */
	if (MSB(req->rid))
		options |= BIT_4;
	/* Use alternate PCI devfn */
	if (LSB(req->rid))
		options |= BIT_5;
	req->options = options;

	ql_dbg(ql_dbg_multiq, base_vha, 0xc003,
	    "options=0x%x.\n", req->options);
	ql_dbg(ql_dbg_init, base_vha, 0x00dd,
	    "options=0x%x.\n", req->options);
	for (cnt = 1; cnt < req->num_outstanding_cmds; cnt++)
		req->outstanding_cmds[cnt] = NULL;
	req->current_outstanding_cmd = 1;

	req->ring_ptr = req->ring;
	req->ring_index = 0;
	req->cnt = req->length;
	req->id = que_id;
	reg = ISP_QUE_REG(ha, que_id);
	req->req_q_in = &reg->isp25mq.req_q_in;
	req->req_q_out = &reg->isp25mq.req_q_out;
	req->max_q_depth = ha->req_q_map[0]->max_q_depth;
	req->out_ptr = (uint16_t *)(req->ring + req->length);
	mutex_unlock(&ha->mq_lock);
	ql_dbg(ql_dbg_multiq, base_vha, 0xc004,
	    "ring_ptr=%p ring_index=%d, "
	    "cnt=%d id=%d max_q_depth=%d.\n",
	    req->ring_ptr, req->ring_index,
	    req->cnt, req->id, req->max_q_depth);
	ql_dbg(ql_dbg_init, base_vha, 0x00de,
	    "ring_ptr=%p ring_index=%d, "
	    "cnt=%d id=%d max_q_depth=%d.\n",
	    req->ring_ptr, req->ring_index, req->cnt,
	    req->id, req->max_q_depth);

	if (startqp) {
		ret = qla25xx_init_req_que(base_vha, req);
		if (ret != QLA_SUCCESS) {
			ql_log(ql_log_fatal, base_vha, 0x00df,
			    "%s failed.\n", __func__);
			mutex_lock(&ha->mq_lock);
			clear_bit(que_id, ha->req_qid_map);
			mutex_unlock(&ha->mq_lock);
			goto que_failed;
		}
		vha->flags.qpairs_req_created = 1;
	}

	return req->id;

que_failed:
	qla25xx_free_req_que(base_vha, req);
failed:
	return 0;
}

static void qla_do_work(struct work_struct *work)
{
	unsigned long flags;
	struct qla_qpair *qpair = container_of(work, struct qla_qpair, q_work);
	struct scsi_qla_host *vha = qpair->vha;

	spin_lock_irqsave(&qpair->qp_lock, flags);
	qla24xx_process_response_queue(vha, qpair->rsp);
	spin_unlock_irqrestore(&qpair->qp_lock, flags);

}

/* create response queue */
int
qla25xx_create_rsp_que(struct qla_hw_data *ha, uint16_t options,
    uint8_t vp_idx, uint16_t rid, struct qla_qpair *qpair, bool startqp)
{
	int ret = 0;
	struct rsp_que *rsp = NULL;
	struct scsi_qla_host *base_vha = pci_get_drvdata(ha->pdev);
	struct scsi_qla_host *vha = pci_get_drvdata(ha->pdev);
	uint16_t que_id = 0;
	device_reg_t *reg;

	rsp = kzalloc(sizeof(struct rsp_que), GFP_KERNEL);
	if (rsp == NULL) {
		ql_log(ql_log_warn, base_vha, 0x0066,
		    "Failed to allocate memory for response queue.\n");
		goto failed;
	}

	rsp->length = RESPONSE_ENTRY_CNT_MQ;
	rsp->ring = dma_alloc_coherent(&ha->pdev->dev,
			(rsp->length + 1) * sizeof(response_t),
			&rsp->dma, GFP_KERNEL);
	if (rsp->ring == NULL) {
		ql_log(ql_log_warn, base_vha, 0x00e1,
		    "Failed to allocate memory for response ring.\n");
		goto que_failed;
	}

	mutex_lock(&ha->mq_lock);
	que_id = find_first_zero_bit(ha->rsp_qid_map, ha->max_rsp_queues);
	if (que_id >= ha->max_rsp_queues) {
		mutex_unlock(&ha->mq_lock);
		ql_log(ql_log_warn, base_vha, 0x00e2,
		    "No resources to create additional request queue.\n");
		goto que_failed;
	}
	set_bit(que_id, ha->rsp_qid_map);

	rsp->msix = qpair->msix;

	ha->rsp_q_map[que_id] = rsp;
	rsp->rid = rid;
	rsp->vp_idx = vp_idx;
	rsp->hw = ha;
	ql_dbg(ql_dbg_init, base_vha, 0x00e4,
	    "rsp queue_id=%d rid=%d vp_idx=%d hw=%p.\n",
	    que_id, rsp->rid, rsp->vp_idx, rsp->hw);
	/* Use alternate PCI bus number */
	if (MSB(rsp->rid))
		options |= BIT_4;
	/* Use alternate PCI devfn */
	if (LSB(rsp->rid))
		options |= BIT_5;
	/* Enable MSIX handshake mode on for uncapable adapters */
	if (!IS_MSIX_NACK_CAPABLE(ha))
		options |= BIT_6;

	/* Set option to indicate response queue creation */
	options |= BIT_1;

	rsp->options = options;
	rsp->id = que_id;
	reg = ISP_QUE_REG(ha, que_id);
	rsp->rsp_q_in = &reg->isp25mq.rsp_q_in;
	rsp->rsp_q_out = &reg->isp25mq.rsp_q_out;
	rsp->in_ptr = (uint16_t *)(rsp->ring + rsp->length);
	mutex_unlock(&ha->mq_lock);
	ql_dbg(ql_dbg_multiq, base_vha, 0xc00b,
	    "options=%x id=%d rsp_q_in=%p rsp_q_out=%p\n",
	    rsp->options, rsp->id, rsp->rsp_q_in,
	    rsp->rsp_q_out);
	ql_dbg(ql_dbg_init, base_vha, 0x00e5,
	    "options=%x id=%d rsp_q_in=%p rsp_q_out=%p\n",
	    rsp->options, rsp->id, rsp->rsp_q_in,
	    rsp->rsp_q_out);

	ret = qla25xx_request_irq(ha, qpair, qpair->msix,
		ha->flags.disable_msix_handshake ?
		QLA_MSIX_QPAIR_MULTIQ_RSP_Q : QLA_MSIX_QPAIR_MULTIQ_RSP_Q_HS);
	if (ret)
		goto que_failed;

	if (startqp) {
		ret = qla25xx_init_rsp_que(base_vha, rsp);
		if (ret != QLA_SUCCESS) {
			ql_log(ql_log_fatal, base_vha, 0x00e7,
			    "%s failed.\n", __func__);
			mutex_lock(&ha->mq_lock);
			clear_bit(que_id, ha->rsp_qid_map);
			mutex_unlock(&ha->mq_lock);
			goto que_failed;
		}
		vha->flags.qpairs_rsp_created = 1;
	}
	rsp->req = NULL;

	qla2x00_init_response_q_entries(rsp);
	if (qpair->hw->wq)
		INIT_WORK(&qpair->q_work, qla_do_work);
	return rsp->id;

que_failed:
	qla25xx_free_rsp_que(base_vha, rsp);
failed:
	return 0;
}

static void qla_ctrlvp_sp_done(srb_t *sp, int res)
{
	if (sp->comp)
		complete(sp->comp);
	/* don't free sp here. Let the caller do the free */
}

/**
 * qla24xx_control_vp() - Enable a virtual port for given host
 * @vha:	adapter block pointer
 * @cmd:	command type to be sent for enable virtual port
 *
 * Return:	qla2xxx local function return status code.
 */
int qla24xx_control_vp(scsi_qla_host_t *vha, int cmd)
{
	int rval = QLA_MEMORY_ALLOC_FAILED;
	struct qla_hw_data *ha = vha->hw;
	int	vp_index = vha->vp_idx;
	struct scsi_qla_host *base_vha = pci_get_drvdata(ha->pdev);
	DECLARE_COMPLETION_ONSTACK(comp);
	srb_t *sp;

	ql_dbg(ql_dbg_vport, vha, 0x10c1,
	    "Entered %s cmd %x index %d.\n", __func__, cmd, vp_index);

	if (vp_index == 0 || vp_index >= ha->max_npiv_vports)
		return QLA_PARAMETER_ERROR;

	/* ref: INIT */
	sp = qla2x00_get_sp(base_vha, NULL, GFP_KERNEL);
	if (!sp)
		return rval;

	sp->type = SRB_CTRL_VP;
	sp->name = "ctrl_vp";
	sp->comp = &comp;
	qla2x00_init_async_sp(sp, qla2x00_get_async_timeout(vha) + 2,
			      qla_ctrlvp_sp_done);
	sp->u.iocb_cmd.u.ctrlvp.cmd = cmd;
	sp->u.iocb_cmd.u.ctrlvp.vp_index = vp_index;

	rval = qla2x00_start_sp(sp);
	if (rval != QLA_SUCCESS) {
		ql_dbg(ql_dbg_async, vha, 0xffff,
		    "%s: %s Failed submission. %x.\n",
		    __func__, sp->name, rval);
		goto done;
	}

	ql_dbg(ql_dbg_vport, vha, 0x113f, "%s hndl %x submitted\n",
	    sp->name, sp->handle);

	wait_for_completion(&comp);
	sp->comp = NULL;

	rval = sp->rc;
	switch (rval) {
	case QLA_FUNCTION_TIMEOUT:
		ql_dbg(ql_dbg_vport, vha, 0xffff, "%s: %s Timeout. %x.\n",
		    __func__, sp->name, rval);
		break;
	case QLA_SUCCESS:
		ql_dbg(ql_dbg_vport, vha, 0xffff, "%s: %s done.\n",
		    __func__, sp->name);
		break;
	default:
		ql_dbg(ql_dbg_vport, vha, 0xffff, "%s: %s Failed. %x.\n",
		    __func__, sp->name, rval);
		break;
	}
done:
	/* ref: INIT */
	kref_put(&sp->cmd_kref, qla2x00_sp_release);
	return rval;
}

struct scsi_qla_host *qla_find_host_by_vp_idx(struct scsi_qla_host *vha, uint16_t vp_idx)
{
	struct qla_hw_data *ha = vha->hw;

	if (vha->vp_idx == vp_idx)
		return vha;

	BUG_ON(ha->vp_map == NULL);
	if (likely(test_bit(vp_idx, ha->vp_idx_map)))
		return ha->vp_map[vp_idx].vha;

	return NULL;
}

/* vport_slock to be held by the caller */
void
qla_update_vp_map(struct scsi_qla_host *vha, int cmd)
{
	void *slot;
	u32 key;
	int rc;

	if (!vha->hw->vp_map)
		return;

	key = vha->d_id.b24;

	switch (cmd) {
	case SET_VP_IDX:
		vha->hw->vp_map[vha->vp_idx].vha = vha;
		break;
	case SET_AL_PA:
		slot = btree_lookup32(&vha->hw->host_map, key);
		if (!slot) {
			ql_dbg(ql_dbg_disc, vha, 0xf018,
			    "Save vha in host_map %p %06x\n", vha, key);
			rc = btree_insert32(&vha->hw->host_map,
			    key, vha, GFP_ATOMIC);
			if (rc)
				ql_log(ql_log_info, vha, 0xd03e,
				    "Unable to insert s_id into host_map: %06x\n",
				    key);
			return;
		}
		ql_dbg(ql_dbg_disc, vha, 0xf019,
		    "replace existing vha in host_map %p %06x\n", vha, key);
		btree_update32(&vha->hw->host_map, key, vha);
		break;
	case RESET_VP_IDX:
		vha->hw->vp_map[vha->vp_idx].vha = NULL;
		break;
	case RESET_AL_PA:
		ql_dbg(ql_dbg_disc, vha, 0xf01a,
		    "clear vha in host_map %p %06x\n", vha, key);
		slot = btree_lookup32(&vha->hw->host_map, key);
		if (slot)
			btree_remove32(&vha->hw->host_map, key);
		vha->d_id.b24 = 0;
		break;
	}
}

void qla_update_host_map(struct scsi_qla_host *vha, port_id_t id)
{

	if (!vha->d_id.b24) {
		vha->d_id = id;
		qla_update_vp_map(vha, SET_AL_PA);
	} else if (vha->d_id.b24 != id.b24) {
		qla_update_vp_map(vha, RESET_AL_PA);
		vha->d_id = id;
		qla_update_vp_map(vha, SET_AL_PA);
	}
}

int qla_create_buf_pool(struct scsi_qla_host *vha, struct qla_qpair *qp)
{
	int sz;

	qp->buf_pool.num_bufs = qp->req->length;

	sz = BITS_TO_LONGS(qp->req->length);
	qp->buf_pool.buf_map   = kcalloc(sz, sizeof(long), GFP_KERNEL);
	if (!qp->buf_pool.buf_map) {
		ql_log(ql_log_warn, vha, 0x0186,
		    "Failed to allocate buf_map(%zd).\n", sz * sizeof(unsigned long));
		return -ENOMEM;
	}
	sz = qp->req->length * sizeof(void *);
	qp->buf_pool.buf_array = kcalloc(qp->req->length, sizeof(void *), GFP_KERNEL);
	if (!qp->buf_pool.buf_array) {
		ql_log(ql_log_warn, vha, 0x0186,
		    "Failed to allocate buf_array(%d).\n", sz);
		kfree(qp->buf_pool.buf_map);
		return -ENOMEM;
	}
	sz = qp->req->length * sizeof(dma_addr_t);
	qp->buf_pool.dma_array = kcalloc(qp->req->length, sizeof(dma_addr_t), GFP_KERNEL);
	if (!qp->buf_pool.dma_array) {
		ql_log(ql_log_warn, vha, 0x0186,
		    "Failed to allocate dma_array(%d).\n", sz);
		kfree(qp->buf_pool.buf_map);
		kfree(qp->buf_pool.buf_array);
		return -ENOMEM;
	}
	set_bit(0, qp->buf_pool.buf_map);
	return 0;
}

void qla_free_buf_pool(struct qla_qpair *qp)
{
	int i;
	struct qla_hw_data *ha = qp->vha->hw;

	for (i = 0; i < qp->buf_pool.num_bufs; i++) {
		if (qp->buf_pool.buf_array[i] && qp->buf_pool.dma_array[i])
			dma_pool_free(ha->fcp_cmnd_dma_pool, qp->buf_pool.buf_array[i],
			    qp->buf_pool.dma_array[i]);
		qp->buf_pool.buf_array[i] = NULL;
		qp->buf_pool.dma_array[i] = 0;
	}

	kfree(qp->buf_pool.dma_array);
	kfree(qp->buf_pool.buf_array);
	kfree(qp->buf_pool.buf_map);
}

/* it is assume qp->qp_lock is held at this point */
int qla_get_buf(struct scsi_qla_host *vha, struct qla_qpair *qp, struct qla_buf_dsc *dsc)
{
	u16 tag, i = 0;
	void *buf;
	dma_addr_t buf_dma;
	struct qla_hw_data *ha = vha->hw;

	dsc->tag = TAG_FREED;
again:
	tag = find_first_zero_bit(qp->buf_pool.buf_map, qp->buf_pool.num_bufs);
	if (tag >= qp->buf_pool.num_bufs) {
		ql_dbg(ql_dbg_io, vha, 0x00e2,
		    "qp(%d) ran out of buf resource.\n", qp->id);
		return  -EIO;
	}
	if (tag == 0) {
		set_bit(0, qp->buf_pool.buf_map);
		i++;
		if (i == 5) {
			ql_dbg(ql_dbg_io, vha, 0x00e3,
			    "qp(%d) unable to get tag.\n", qp->id);
			return -EIO;
		}
		goto again;
	}

	if (!qp->buf_pool.buf_array[tag]) {
		buf = dma_pool_zalloc(ha->fcp_cmnd_dma_pool, GFP_ATOMIC, &buf_dma);
		if (!buf) {
			ql_log(ql_log_fatal, vha, 0x13b1,
			    "Failed to allocate buf.\n");
			return -ENOMEM;
		}

		dsc->buf = qp->buf_pool.buf_array[tag] = buf;
		dsc->buf_dma = qp->buf_pool.dma_array[tag] = buf_dma;
		qp->buf_pool.num_alloc++;
	} else {
		dsc->buf = qp->buf_pool.buf_array[tag];
		dsc->buf_dma = qp->buf_pool.dma_array[tag];
		memset(dsc->buf, 0, FCP_CMND_DMA_POOL_SIZE);
	}

	qp->buf_pool.num_active++;
	if (qp->buf_pool.num_active > qp->buf_pool.max_used)
		qp->buf_pool.max_used = qp->buf_pool.num_active;

	dsc->tag = tag;
	set_bit(tag, qp->buf_pool.buf_map);
	return 0;
}

static void qla_trim_buf(struct qla_qpair *qp, u16 trim)
{
	int i, j;
	struct qla_hw_data *ha = qp->vha->hw;

	if (!trim)
		return;

	for (i = 0; i < trim; i++) {
		j = qp->buf_pool.num_alloc - 1;
		if (test_bit(j, qp->buf_pool.buf_map)) {
			ql_dbg(ql_dbg_io + ql_dbg_verbose, qp->vha, 0x300b,
			       "QP id(%d): trim active buf[%d]. Remain %d bufs\n",
			       qp->id, j, qp->buf_pool.num_alloc);
			return;
		}

		if (qp->buf_pool.buf_array[j]) {
			dma_pool_free(ha->fcp_cmnd_dma_pool, qp->buf_pool.buf_array[j],
				      qp->buf_pool.dma_array[j]);
			qp->buf_pool.buf_array[j] = NULL;
			qp->buf_pool.dma_array[j] = 0;
		}
		qp->buf_pool.num_alloc--;
		if (!qp->buf_pool.num_alloc)
			break;
	}
	ql_dbg(ql_dbg_io + ql_dbg_verbose, qp->vha, 0x3010,
	       "QP id(%d): trimmed %d bufs. Remain %d bufs\n",
	       qp->id, trim, qp->buf_pool.num_alloc);
}

static void __qla_adjust_buf(struct qla_qpair *qp)
{
	u32 trim;

	qp->buf_pool.take_snapshot = 0;
	qp->buf_pool.prev_max = qp->buf_pool.max_used;
	qp->buf_pool.max_used = qp->buf_pool.num_active;

	if (qp->buf_pool.prev_max > qp->buf_pool.max_used &&
	    qp->buf_pool.num_alloc > qp->buf_pool.max_used) {
		/* down trend */
		trim = qp->buf_pool.num_alloc - qp->buf_pool.max_used;
		trim  = (trim * 10) / 100;
		trim = trim ? trim : 1;
		qla_trim_buf(qp, trim);
	} else if (!qp->buf_pool.prev_max  && !qp->buf_pool.max_used) {
		/* 2 periods of no io */
		qla_trim_buf(qp, qp->buf_pool.num_alloc);
	}
}

/* it is assume qp->qp_lock is held at this point */
void qla_put_buf(struct qla_qpair *qp, struct qla_buf_dsc *dsc)
{
	if (dsc->tag == TAG_FREED)
		return;
	lockdep_assert_held(qp->qp_lock_ptr);

	clear_bit(dsc->tag, qp->buf_pool.buf_map);
	qp->buf_pool.num_active--;
	dsc->tag = TAG_FREED;

	if (qp->buf_pool.take_snapshot)
		__qla_adjust_buf(qp);
}

#define EXPIRE (60 * HZ)
void qla_adjust_buf(struct scsi_qla_host *vha)
{
	unsigned long flags;
	int i;
	struct qla_qpair *qp;

	if (vha->vp_idx)
		return;

	if (!vha->buf_expired) {
		vha->buf_expired = jiffies + EXPIRE;
		return;
	}
	if (time_before(jiffies, vha->buf_expired))
		return;

	vha->buf_expired = jiffies + EXPIRE;

	for (i = 0; i < vha->hw->num_qpairs; i++) {
		qp = vha->hw->queue_pair_map[i];
		if (!qp)
			continue;
		if (!qp->buf_pool.num_alloc)
			continue;

		if (qp->buf_pool.take_snapshot) {
			/* no io has gone through in the last EXPIRE period */
			spin_lock_irqsave(qp->qp_lock_ptr, flags);
			__qla_adjust_buf(qp);
			spin_unlock_irqrestore(qp->qp_lock_ptr, flags);
		} else {
			qp->buf_pool.take_snapshot = 1;
		}
	}
}
