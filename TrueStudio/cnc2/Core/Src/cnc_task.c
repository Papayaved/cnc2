#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <ctype.h>
#include <fpga_gpo.h>
#include <math.h>
#include <limits.h>

#include "cnc_task.h"
#include "key_task.h"
#include "line.h"
#include "arc.h"
#include "fpga.h"
#include "fpga_gpo.h"
#include "controls.h"
#include "step_dir.h"
#include "imit_fifo.h"
#include "prog_array.h"
#include "my_lib.h"
#include "aux_func.h"
#include "cnc_func.h"
#include "uv.h"
#include "context.h"
#include "backup.h"
#include "feedback.h"
#include "encoder.h"
#include "soft_wdt.h"
#include "pid.h"
#include "center.h"
#include "cnc_param.h"
#include "center.h"

void reset();

typedef enum {ST_IDLE, ST_DIRECT, ST_CENTER, ST_READ, ST_SEGMENT, ST_WAIT, ST_PAUSE, ST_STOP, ST_WAIT_BUTTON, ST_END, ST_ERROR} cnc_state_t;
typedef enum {ST_ALM_NORMAL, ST_ALM_POWER, ST_ALM_WIRE, ST_ALM_ALARM, ST_ALM_DRUM, ST_ALM_DRUM_ERROR, ST_ALM_SOFT} alarm_state_t;

// supported  codes: "G0", "G1", "G4", "G92", "M2", "M30"
static cnc_state_t state, state_old, state_reg; // current, print, restore
static alarm_state_t alm_state;

static uint16_t flags, limsw_reg;
static BOOL hv_enabled;
static volatile BOOL run_req, stop_req, cancel_req, imit_ena;

static double T = T_DEFAULT_TICK, T_reg = T_DEFAULT_TICK; // ticks / mm
static float T_cur = FLT_MAX; // clock / mm
static uint32_t T_pause;

static BOOL update_pos_req;
static point_t mtr_pt, mtr_uv_pt;

static cnc_param_t cnc_par; // ???

static BOOL init;
static int init_cnt;

static line_t line, uv_line;
static arc_t arc, uv_arc;
static int N_reg = -1;
static size_t step_id;

static volatile BOOL rev_req;
static BOOL rb_req, rb_ena, rev;
static path_t rb_path;
static double rb_len = ROLLBACK_DEFAULT;
static unsigned rb_attempts = ROLLBACK_ATTEMPTS, rb_attempt;

static BOOL fault;

BOOL cnc_fault() { return fault || soft_wdt(); }

void cnc_clearDirectParam(cnc_param_t* const par) {
	memset(par, 0, sizeof(cnc_param_t));
}

// CNC DIRECT
static int32_t param_reg[2*MOTORS]; // motors + encoders
static uint8_t direct_valid;

void cnc_setParam(size_t index, int32_t value) {
	if (index < sizeof(param_reg)/sizeof(param_reg[0])) {
		param_reg[index] = value;
		direct_valid |= 1 << index;
	}
	else
		direct_valid = 0;
}

int32_t cnc_getParam(size_t index) {
	if (index < sizeof(param_reg)/sizeof(param_reg[0]))
		return param_reg[index];

	return 0;
}

void cnc_recovery() {
	if (state == ST_IDLE && direct_valid == 0x3f) {
//		fpga_setRunEnable(FALSE); // don't need after fpga reset
		enc_disable();

		mtr_pt.x = param_reg[0];
		mtr_pt.y = param_reg[1];
		mtr_uv_pt.x = param_reg[2];
		mtr_uv_pt.y = param_reg[3];

		fpga_setPos(0, param_reg[0]);
		fpga_setPos(1, param_reg[1]);
		fpga_setPos(2, param_reg[2]);
		fpga_setPos(3, param_reg[3]);

		enc_setXY(param_reg[4], param_reg[5]);

		enc_enable();
//		fpga_setRunEnable(TRUE);

		update_pos_req = TRUE;

		printf("G92_req (%d %d %d %d) ENC(%d %d)\n", (int)mtr_pt.x, (int)mtr_pt.y, (int)mtr_uv_pt.x, (int)mtr_uv_pt.y, (int)param_reg[4], (int)param_reg[5]);
	}

	direct_valid = 0;
}

void cnc_reqG92() {
	if (state == ST_IDLE && ((direct_valid & 0xf) == 0xf)) {
		pa_gotoBegin();

		mtr_pt.x = param_reg[0];
		mtr_pt.y = param_reg[1];
		mtr_uv_pt.x = param_reg[2];
		mtr_uv_pt.y = param_reg[3];

		fpga_setSoftPermit(FALSE);

		fpga_setPos(0, param_reg[0]);
		fpga_setPos(1, param_reg[1]);
		fpga_setPos(2, param_reg[2]);
		fpga_setPos(3, param_reg[3]);

		if ((direct_valid & 0x30) == 0x30)
			enc_setXY(param_reg[4], param_reg[5]);

		fpga_setSoftPermit(TRUE);

		if (direct_valid == 0x3f)
			printf("G92_req (%d %d %d %d) ENC(%d %d)\n", (int)mtr_pt.x, (int)mtr_pt.y, (int)mtr_uv_pt.x, (int)mtr_uv_pt.y, (int)param_reg[4], (int)param_reg[5]);
		else
			printf("G92_req (%d %d %d %d)\n", (int)mtr_pt.x, (int)mtr_pt.y, (int)mtr_uv_pt.x, (int)mtr_uv_pt.y);
	}

	direct_valid = 0;
}

void cnc_reqG92_xyuv_enc(int32_t x, int32_t y, int32_t u, int32_t v, int32_t enc_x, int32_t enc_y) {
	if (state == ST_IDLE) {
		pa_gotoBegin();

		mtr_pt.x = x;
		mtr_pt.y = y;
		mtr_uv_pt.x = u;
		mtr_uv_pt.y = v;

		fpga_setSoftPermit(FALSE);

		fpga_setPos(0, x);
		fpga_setPos(1, y);
		fpga_setPos(2, u);
		fpga_setPos(3, v);
		enc_setXY(enc_x, enc_y);

		fpga_setSoftPermit(TRUE);

		printf("G92_req (%d %d %d %d) ENC(%d %d)\n", (int)mtr_pt.x, (int)mtr_pt.y, (int)mtr_uv_pt.x, (int)mtr_uv_pt.y, (int)enc_x, (int)enc_y);
	}
}

void cnc_reqG1() {
	if (state == ST_IDLE && direct_valid == 0xff) {
		pa_gotoBegin();

		int32_t nx = param_reg[0];
		int32_t Tx = param_reg[1];
		int32_t ny = param_reg[2];
		int32_t Ty = param_reg[3];
		int32_t nu = param_reg[4];
		int32_t Tu = param_reg[5];
		int32_t nv = param_reg[6];
		int32_t Tv = param_reg[7];

		uint16_t wrreq = 0;

		if (nx) {
			if (Tx < T_MIN) Tx = T_MIN;
			fpga_step(0, nx, Tx);
			wrreq |= 1;
		}

		if (ny) {
			if (Ty < T_MIN) Ty = T_MIN;
			fpga_step(1, ny, Ty);
			wrreq |= 2;
		}

		if (nu) {
			if (Tu < T_MIN) Tu = T_MIN;
			fpga_step(2, nu, Tu);
			wrreq |= 4;
		}

		if (nv) {
			if (Tv < T_MIN) Tv = T_MIN;
			fpga_step(3, nv, Tv);
			wrreq |= 8;
		}

		if (wrreq) {
			fpga_setTaskID(0);
			fpga_write_u16(MTR_WRREQ>>1, wrreq);
		}

		mtr_pt.x += nx;
		mtr_pt.y += ny;
		mtr_uv_pt.x += nu;
		mtr_uv_pt.y += nv;

		state = ST_DIRECT; // initialization reset it
		printf("G1_req (%d %d %d %d) T(%d %d %d %d)\n",
				(int)mtr_pt.x, (int)mtr_pt.y, (int)mtr_uv_pt.x, (int)mtr_uv_pt.y,
				(int)Tx, (int)Ty, (int)Tu, (int)Tv
			);
	}

	direct_valid = 0;
}

void cnc_reqCenter(const center_t* const pcenter) {
	if (state == ST_IDLE) {
		pa_gotoBegin();
		cnc_reset();

		if (center_init(pcenter) == 0)
			if (center_next(&T_cur) == 0)
				state = ST_CENTER;
	}
}

// for recovery
void cnc_enableUV(BOOL value) {
	if (state == ST_IDLE)
		pa_enableUV(value);
}

BOOL cnc_getUVEnabled() {
	return pa_plane() == PLANE_XYUV;
}

void cnc_clear1() {
	state = state_old = state_reg = ST_IDLE;
	alm_state = ST_ALM_NORMAL;
	flags = limsw_reg = 0;
	hv_enabled = FALSE;
	run_req = stop_req = cancel_req = imit_ena = rev_req = FALSE;

	T_cur = FLT_MAX;
	T_pause = 0;

	cnc_clearDirectParam(&cnc_par);
	line_clear(&line);
	arc_clear(&arc);
	N_reg = -1;
	step_id = 0;

	rb_req = rb_ena = rev = FALSE;
	path_clear(&rb_path);
	rb_attempt = 0;

	direct_valid = 0;

	pid_clear();
}

void cnc_clear() {
	cnc_clear1();
	rb_len = ROLLBACK_DEFAULT;
	rb_attempts = ROLLBACK_ATTEMPTS;
	T = T_reg = T_DEFAULT_TICK;
}

void cnc_reset() {
	fpga_init();

	pid_reset();
	fb_reset();

	pa_clear();

	cnc_clear();

	update_pos_req = FALSE;
	memset(&mtr_pt, 0, sizeof(mtr_pt));
	memset(&mtr_uv_pt, 0, sizeof(mtr_uv_pt));

	init = FALSE;
	init_cnt = 0;
	key_task_reset();
	soft_wdt_enable(FALSE);
}

BOOL cnc_isRollback() { return rb_ena; }
BOOL cnc_isReverse() { return rev; }

// motor must be stopped
static void cnc_setRevForce(BOOL ena) {
	rb_path.x = 0;
	rb_path.y = 0;

	rev = ena != 0;

	context_t ctx = step_getContext();

	mtr_pt = ctx.pt;
	mtr_uv_pt = ctx.uv_pt;

	update_pos_req = TRUE;

	pa_goto(ctx.str_num);
}

// Function change reverse bit
// result: TRUE - request state = READ
static BOOL cnc_setRev(BOOL ena) {
	ena = ena != 0;

	if (rev ^ ena) { // Enable or disable
		cnc_setRevForce(ena);
		return TRUE;
	}

	rb_path.x = 0;
	rb_path.y = 0;

	return FALSE;
}

// Recovery
void cnc_goto(uint32_t str_num) {
	if (state == ST_IDLE) {
		const char* str = pa_goto(str_num);
		if (!str) {
			printf("Goto ERR1\n");
			return;
		}

//		fpga_init();
//		cnc_clear();
	//	key_task_reset(); // ?

		update_pos_req = TRUE; // init step_id

		state = ST_STOP;
		state_reg = ST_READ; // init segment

#ifdef SOFT_WDT
		soft_wdt_enable(TRUE);
#endif

		N_reg = pa_getStrNum();
		step_writePause(N_reg, 1000); // 0.1 sec

		printf("Goto %d\n", N_reg);
	}
	else
		printf("Goto ERR2\n");
}

// return TRUE if state was changed
static BOOL cnc_rb() {
	if (rb_ena) {
		if ( steps_to_mm(rb_path.x, cnc_scale_x()) + steps_to_mm(rb_path.y, cnc_scale_y()) >= rb_len ) {
			rb_path.x = rb_path.y = 0;

			uint16_t high = fpga_getHighThld();
			fpga_adcSnapshop();
			uint16_t adc = fpga_getADC(0);

			if (adc != ADC_ERROR && adc >= high) { // rollback cancel
#ifdef PRINT
				printf("RB Stop\n");
				printf("ATT %d/%d\n", rb_attempt, rb_attempts);
#endif

				rb_ena = FALSE;
				rb_attempt = 0;

				cnc_setRevForce(FALSE);
				fb_enableForce(TRUE);
				fpga_clearTimeout();

				state = ST_READ;
				return TRUE;
			}
			else {
#ifdef PRINT
				printf("ATT %d/%d\n", rb_attempt, rb_attempts);
#endif

				if (rb_attempt >= rb_attempts) { // Alert
#ifdef PRINT
					printf("RB Alert\n");
#endif

					fpga_setSoftAlarm();

					rb_ena = FALSE;
					rb_attempt = 0;

					cnc_setRevForce(FALSE);
					fb_enableForce(TRUE);
					fpga_clearTimeout();

					state = ST_STOP;
					state_reg = ST_READ;
					rev_req = FALSE; // ?

					return TRUE;
				}
				else { // rollback continue
//					cnc_setRevForce(TRUE);
#ifdef PRINT
					printf("RB Continue\n");
#endif
				}

				++rb_attempt;
			}
		}
	}
	else if (rb_req) {
		rb_req = FALSE;

		fb_enableForce(FALSE);
		fpga_clearTimeout();

		rb_ena = TRUE; // rb_attempt != 0
		rb_attempt = 1;

		cnc_setRevForce(TRUE);

		state = ST_READ;
		return TRUE;
	}

	return FALSE;
}

static void onAlarm() {
	gpo_setVoltageEnable(FALSE);
	gpo_setDrumEnable(FALSE);
	gpo_setPumpEnable(FALSE);
	gpo_store();

	gpo_readControls();
	gpo_setVoltageEnable(FALSE);
//	gpo_setDrumEnableM(FALSE); // fpga auto
	gpo_setPumpEnable(FALSE);
	gpo_apply();

	stop_req = TRUE;
}

static void offAlarm() {
	gpo_restore();
	gpo_apply();
	fpga_clearSoftAlarm();
}

static void cnc_exit() {
	fpga_setSoftAlarm();
	fpga_motorAbort();
	gpo_readControls();
	cnc_turnOff();

	cnc_clear1();
	fb_enableRestore();
}

BOOL cnc_isLocked() { return (alm_state != ST_ALM_NORMAL && alm_state != ST_ALM_DRUM) || !init; };
BOOL cnc_isNormal() { return alm_state == ST_ALM_NORMAL && init; };
BOOL cnc_isInit() { return init; };

BOOL cnc_drumPermit() { return (alm_state == ST_ALM_NORMAL || alm_state == ST_ALM_DRUM) && init; }

static void alarm_sm();
static void motor_sm();

void cnc_task() {
	static uint16_t limsw_old;

	flags = fpga_getFlags();
	limsw_reg = fpga_getLimSwReg();

	hv_enabled = (flags & FLAG_HV_ENA_MSK) != 0;

	// shortcut
	if (hv_enabled && (flags & (FLAG_NO_PERMIT_MSK | FLAG_TIMEOUT_MSK))) {
#ifdef PRINT
		static uint16_t flags_reg;
		if (flags != flags_reg) {
			printf("AVG F:%x\n", flags);
			flags_reg = flags;
		}
#endif
		pid_clear();
	}

	flags &= FLAG_LIMSW_MSK | FLAG_NO_PERMIT_MSK | FLAG_TIMEOUT_MSK;
	fault = (limsw_reg & (LIM_POWER_MSK | LIM_WIRE_MSK | LIM_ALARM_MSK)) != 0;

//	uint16_t limsw = fpga_getLimSw();
//	uint32_t key = fpga_getKeys();

	if (init) {
		alarm_sm();
		motor_sm();
		return;
	}

	BOOL fwd = (limsw_reg & LIM_FWD_MSK) != 0;
	BOOL rev = (limsw_reg & LIM_REV_MSK) != 0;

	if  (gpo_cncEnabled() && !(fwd && rev) && !fault) {
		init = TRUE;
#ifdef PRINT
		printf("CNC init\n");
#endif
	}
	else {
		if (init_cnt <= 1) {
#ifdef PRINT
			if (init_cnt == 1)
				printf("CNC init ERROR (limsw:%x)\n", limsw_reg);
#endif

			init_cnt++;
		} else if (limsw_reg != limsw_old) {
#ifdef PRINT
			printf("CNC init ERROR (limsw:%x)\n", limsw_reg);
#endif
		}
	}

	fpga_clearLimSwReg(limsw_reg);
	limsw_old = limsw_reg;
}

void cnc_onLoseConnection() {
//	fpga_setSoftAlarm();

	if (cnc_run()) {
		cnc_context_t* ctx = cnc_ctx_getForce();
		bkp_saveContextReg(ctx);
	}

	init = FALSE; // or reset all?

	alm_state = ST_ALM_POWER;
	onAlarm();

	printf("Lose connection\n");
}


// Alarm state machine
static void alarm_sm() {
	limsw_reg &= LIM_SOFT_MSK | LIM_POWER_MSK | LIM_WIRE_MSK | LIM_ALARM_MSK | LIM_REV_MSK | LIM_FWD_MSK;
	fpga_clearLimSwReg(limsw_reg); // clear don't work if limsw still valid

//		if (limsw_reg) {
//			uint16_t limsw = fpga_getLimSw();
//			printf("F:%x LIM:%04x(REG:%04x)\n", (unsigned)flags, (unsigned)limsw, (unsigned)limsw_reg);
//
//			uint16_t flags = fpga_getFlags() & (FLAG_LIMSW_MASK | FLAG_TIMEOUT_MASK);
//			limsw = fpga_getLimSw();
//			uint16_t limsw_reg = fpga_getLimSwReg();
//			printf("F:%x LIM:%04x(REG:%04x)\n", (unsigned)flags, (unsigned)limsw, (unsigned)limsw_reg);
//		}

//	ena = gpo_getCncEnable();

//		if ((limsw_reg & ~LIM_SOFT_MSK) != 0 && !(limsw_reg | LIM_SOFT_MSK)) // if hard disable and no soft alarm
//			fpga_setSoftAlarm();

	if ((limsw_reg & LIM_POWER_MSK) && alm_state != ST_ALM_POWER) { // fpga auto stop
		if (cnc_run()) {
			cnc_context_t* ctx = cnc_ctx_getForce();
			bkp_saveContextReg(ctx);
		}

		init = FALSE; // or reset all?

		alm_state = ST_ALM_POWER;
		onAlarm();

		printf("Power off\n");
	}
	else if ((limsw_reg & LIM_ALARM_MSK) && alm_state != ST_ALM_ALARM) {
		alm_state = ST_ALM_ALARM;
		onAlarm();
		printf("Alarm on\n");
	}
	else if ((limsw_reg & LIM_WIRE_MSK) && alm_state != ST_ALM_WIRE) {
		alm_state = ST_ALM_WIRE;
		onAlarm();
		printf("Wire off\n");
	}
	else if ((limsw_reg & LIM_SOFT_MSK) && alm_state != ST_ALM_SOFT) {
		alm_state = ST_ALM_SOFT;
		onAlarm();
		printf("ST_ALM_SOFT\n");
	}
	else {
		switch (alm_state) {
		case ST_ALM_NORMAL:
			if (limsw_reg & (LIM_FWD_MSK | LIM_REV_MSK)) {
				drum_state_t drum_state = gpo_getDrumState();
				BOOL drum_ena = gpo_getDrumEnable();

				gpo_store();
				gpo_readControls();
				gpo_updateIndicator();

				if (	(limsw_reg & (LIM_FWD_MSK | LIM_REV_MSK)) == (LIM_FWD_MSK | LIM_REV_MSK) ||
						(drum_state == DRUM_FWD && (limsw_reg & LIM_REV_MSK)) ||
						(drum_state == DRUM_REV && (limsw_reg & LIM_FWD_MSK))
				) {
					cnc_stopReq();
					gpo_setPumpEnable(FALSE);

					gpo_setDrumState(DRUM_DIS);
					gpo_apply();
					alm_state = ST_ALM_DRUM_ERROR;
					printf("Drum LIM ERR1\n");
				}
				else if (limsw_reg & LIM_FWD_MSK) {
					if (drum_ena) {
						gpo_setDrumState(DRUM_REV);
						gpo_apply();
					}
					else
						gpo_setDrumStateReg(DRUM_REV);

					alm_state = ST_ALM_DRUM;
					printf("Drum LIM FWD\n");
				}
				else { // (limsw_reg & LIM_REV_MSK)
					if (drum_ena) {
						gpo_setDrumState(DRUM_FWD);
						gpo_apply();
					}
					else
						gpo_setDrumStateReg(DRUM_FWD);

					alm_state = ST_ALM_DRUM;
					printf("Drum LIM REV\n");
				}

				pid_stop();
			}
			else if (limsw_reg) {
				cnc_exit();
				printf("ALM ERROR, %x\n", limsw_reg);
			}

			break;

		case ST_ALM_POWER:
			if (!(limsw_reg & LIM_POWER_MSK)) {
				alm_state = ST_ALM_NORMAL;
				offAlarm();
				// run req clearBackup
				printf("Power on\n");
			}
			break;

		case ST_ALM_WIRE:
			if (!(limsw_reg & LIM_WIRE_MSK)) {
				alm_state = ST_ALM_NORMAL;
				offAlarm();
				printf("Wire on\n");
			}
			break;

		case ST_ALM_ALARM:
			if (!(limsw_reg & LIM_ALARM_MSK)) {
				alm_state = ST_ALM_NORMAL;
				offAlarm();
				printf("Alarm off\n");
			}
			break;

		case ST_ALM_DRUM:
		case ST_ALM_DRUM_ERROR:
			if (!(limsw_reg & (LIM_FWD_MSK | LIM_REV_MSK))) { // exit - no limit switches
				alm_state = ST_ALM_NORMAL;
				drum_state_t drum_state = gpo_getDrumState();
	//				uint8_t vel = gpo_getDrumVel();
				gpo_restore();
				gpo_setDrumState(drum_state);
	//				gpo_setDrumVel(vel);
				gpo_apply();
				fpga_clearSoftAlarm();
				printf("Drum lim on\n");
			}
			else if ( (limsw_reg & (LIM_FWD_MSK | LIM_REV_MSK)) == (LIM_FWD_MSK | LIM_REV_MSK) ) { // error - both limit switches
				cnc_stopReq();
				gpo_setPumpEnable(FALSE);

				gpo_setDrumState(DRUM_DIS);
				gpo_apply();
				alm_state = ST_ALM_DRUM_ERROR;
				printf("Drum LIM ERR2\n");
			}
			else {
				drum_state_t drum_state = gpo_getDrumState();

				if (	(drum_state == DRUM_FWD && (limsw_reg & LIM_FWD_MSK)) ||
						(drum_state == DRUM_REV && (limsw_reg & LIM_REV_MSK))
				) { // incorrect behavior
					cnc_stopReq();
					gpo_setPumpEnable(FALSE);

					gpo_setDrumState(DRUM_DIS);
					gpo_apply();
					alm_state = ST_ALM_DRUM_ERROR;
					printf("Drum LIM ERR3\n");
				}
			}
			break;

		case ST_ALM_SOFT:
			if (state == ST_IDLE || state == ST_STOP) {
				alm_state = ST_ALM_NORMAL;
				offAlarm();
				printf("CNC ena\n");
			}
			break;

		default:
			cnc_exit();
			alm_state = ST_ALM_NORMAL;
			printf("ERROR alm_state\n");
			break;
		}
	}
}

// Motor State Machine
static void motor_sm() {
	static gcmd_t cmd;

#ifdef RB_TEST
	rev_req = test_req && path.x >= 15;
#endif

	state_old = state;

	if (state != ST_IDLE)
		direct_valid = 0;

	// ADC timeout
	if (flags & FLAG_TIMEOUT_MSK)
		rb_req = cnc_run();
	else
		rb_req = cnc_run() && rb_req; // internal request (for test)

	BOOL lock = cnc_isLocked();

	if (cancel_req)
		cnc_exit();
	else if (stop_req) {
		run_req = stop_req = FALSE;

		if (state == ST_SEGMENT || state == ST_PAUSE || state == ST_WAIT || state == ST_READ) {
			state_reg = state;
			state = ST_STOP;
		}
		else if (state == ST_END)
			cnc_exit();
	}
	else if (lock)
		run_req = FALSE; // ??

	if (lock)
		return;

	switch (state) {
	case ST_IDLE: {
		soft_wdt_enable(FALSE);

		if (run_req) {
			run_req = FALSE;

			gpo_apply();
			fpga_motorAbort();
			fpga_clearSoftAlarm();
			fpga_clearTimeout();
			pa_gotoBegin();
			pid_clear();

//					if (rev_req) {
//						rev_req = FALSE;
//						pa_switchRev();
//					}

			if (pa_init() == 0) {
				BOOL OK = pa_getGCmd(&cmd, NULL);
				const char* s = pa_next();

				if (OK && s && cmd.valid.flag.PCT && cmd.valid.flag.EoF) {
					step_clear();
//					fpga_setMotorOE(TRUE); // it's controlled from PC
#ifdef SOFT_WDT
					soft_wdt_enable(TRUE);
#endif
					state = ST_READ;
				}
			}
		}
	}
		break;

	case ST_DIRECT:
		if (step_isStopped()) {
			state = ST_IDLE;
//			cnc_printState();
		}

#ifdef PRINT
		uint16_t res = fpga_getRun();
		printf("RUN:%04x\n", res);

		int x = fpga_getPos(0);
		int y = fpga_getPos(1);
		int u = fpga_getPos(2);
		int v = fpga_getPos(3);

		int enc_x = enc_get(0);
		int enc_y = enc_get(1);
		printf("(%d %d %d %d) ENC(%d %d)\n", x, y, u, v, enc_x, enc_y);
#endif
		break;

	case ST_CENTER:
		if (center_next(&T_cur) != 0)
			state = ST_IDLE;

		break;

	case ST_STOP:
		if (run_req) {
			run_req = FALSE;

			BOOL revChanged = cnc_setRev(rev_req);
			rev_req = FALSE;

			if (revChanged)
				state = ST_READ;
			else
				state = state_reg;

			if (rev)
				fb_enableForce(FALSE);
			else
				fb_enableRestore();
		}
		break;

	case ST_READ: // Read frame
	{
		static gline_t gline, uv_gline;
		static garc_t garc, uv_garc;

		line_clear(&line); // clear last
		arc_clear(&arc);

		if (!pa_getSegment(&cmd, &gline, &uv_gline, &garc, &uv_garc)) {
			state = ST_ERROR;
			break;
		}

		N_reg = pa_getStrNum(); // for context

		// Speed
		if (cmd.valid.flag.F)
			cnc_setSpeed(cmd.F);
		else
			T = T_reg;

		if (cmd.valid.flag.PCT) {
			step_write(0, 0, 0, TRUE, 0, 0);
			state = ST_END;
#ifdef PRINT
			printf("END\n");
#endif
		}
		else if (cmd.valid.flag.M)
			cnc_setMCmd(&cmd);
		else if (cmd.valid.flag.G) {
			switch (cmd.G) {
			case 0: case 1:
				if (gline.valid) {
					if (cmd.G == 0)
						T = T_DEFAULT_TICK;

					line_init(&line, &gline.A, &gline.B, FALSE, cnc_step());

					if (line.valid) {
						if (rev)
							line_swap(&line);

						if (update_pos_req) {
							update_pos_req = FALSE;

							if (pa_plane() == PLANE_XYUV) {
								fpoint_t mtr_mm = steps_to_fpoint_mm(&mtr_pt, cnc_scale_xy());
								fpoint_t mtr_uv_mm = steps_to_fpoint_mm(&mtr_uv_pt, cnc_scale_uv());
								fpoint_t xy_mm = uv_motors_to_XY(&mtr_mm, &mtr_uv_mm);
								step_id = line_getPos(&line, &xy_mm);
							}
							else {
								fpoint_t xy_mm = steps_to_fpoint_mm(&mtr_pt, cnc_scale_xy());
								step_id = line_getPos(&line, &xy_mm);
							}
						}
						else
							step_id = 0;

						if (!line_isPoint(&line, cnc_scale_xy())) {
							if (pa_plane() == PLANE_XY)
								state = ST_SEGMENT;
							else if (pa_plane() == PLANE_XYUV && cmd.valid.flag.G2) {
								switch (cmd.G2) {
								case 0: case 1:
									line_init(&uv_line, &uv_gline.A, &uv_gline.B, FALSE, cnc_step());
									line_setStepRatio(&uv_line, line.length);

									if (rev)
										line_swap(&uv_line);

									state = ST_SEGMENT;
									break;
								case 2: case 3:
									arc_initCenter(&uv_arc, &uv_garc.A, &uv_garc.B, &uv_garc.C, uv_garc.flag.ccw, cnc_step(), cnc_scale_uv());
									arc_setStepRatio(&uv_arc, line.length);

									if (rev)
										arc_swap(&uv_arc);

									state = ST_SEGMENT;
									break;
								default:
									break;
								}
							}
						}
					}
				}
				break;

			case 2: case 3:
				if (garc.flag.valid && !garc.flag.R) {
					arc_initCenter(&arc, &garc.A, &garc.B, &garc.C, garc.flag.ccw, cnc_step(), cnc_scale_xy());

					if (arc.flag.valid && !arc.flag.empty) {
						if (rev)
							arc_swap(&arc);

						if (update_pos_req) {
							update_pos_req = FALSE;

							if (pa_plane() == PLANE_XYUV) {
								fpoint_t mtr_mm = steps_to_fpoint_mm(&mtr_pt, cnc_scale_xy());
								fpoint_t mtr_uv_mm = steps_to_fpoint_mm(&mtr_uv_pt, cnc_scale_uv());
								fpoint_t xy_mm = uv_motors_to_XY(&mtr_mm, &mtr_uv_mm);
								step_id = arc_getPos(&arc, &xy_mm, cnc_scale_xy());
							}
							else {
								fpoint_t xy_mm = steps_to_fpoint_mm(&mtr_pt, cnc_scale_xy());
								step_id = arc_getPos(&arc, &xy_mm, cnc_scale_xy());
							}
						}
						else
							step_id = 0;

						if (pa_plane() == PLANE_XY)
							state = ST_SEGMENT;
						else if (pa_plane() == PLANE_XYUV && cmd.valid.flag.G2) {
							switch (cmd.G2) {
							case 1:
								line_init(&uv_line, &uv_gline.A, &uv_gline.B, FALSE, cnc_step());
								line_setStepRatio(&uv_line, arc_length(&arc));

								if (rev)
									line_swap(&uv_line);

								state = ST_SEGMENT;
								break;
							case 2: case 3:
								arc_initCenter(&uv_arc, &uv_garc.A, &uv_garc.B, &uv_garc.C, uv_garc.flag.ccw, cnc_step(), cnc_scale_uv());
								arc_setStepRatio(&uv_arc, arc_length(&arc));

								if (rev)
									arc_swap(&uv_arc);

								state = ST_SEGMENT;
								break;
							default:
								break;
							}
						}
					}
				}
				break;

			case 4:
				if (cmd.valid.flag.P) {
					T_pause = fpga_MsToPauseTicks( gcmd_P(&cmd) );
					state = ST_PAUSE;
				}
				break;

			case 92:
				if (pa_plane() == PLANE_XY && cmd.valid.flag.X && cmd.valid.flag.Y) {
					mtr_pt.x = mm_to_steps(cmd.X, cnc_scale_x());
					mtr_pt.y = mm_to_steps(cmd.Y, cnc_scale_y());
					mtr_uv_pt.x = 0;
					mtr_uv_pt.y = 0;
					step_setPos(N_reg, &mtr_pt, &mtr_uv_pt); // only if no Run

					point_t enc_pt = { mm_to_steps(cmd.X, cnc_scale_enc_x()), mm_to_steps(cmd.Y, cnc_scale_enc_y()) };
					enc_setXY(enc_pt.x, enc_pt.y);
				}
				else if (pa_plane() == PLANE_XYUV && cmd.valid.flag.X && cmd.valid.flag.Y && cmd.valid.flag.U && cmd.valid.flag.V) {
					fpoint_t xy_mm = {cmd.X, cmd.Y};
					fpoint_t uv_mm = {cmd.U, cmd.V};

					fpoint_t mtr_mm		= uv_XY_to_motors(&xy_mm, &uv_mm);
					fpoint_t mtr_uv_mm	= uv_UV_to_motors(&xy_mm, &uv_mm);

					mtr_pt		= fpoint_mm_to_steps(&mtr_mm, cnc_scale_xy());
					mtr_uv_pt	= fpoint_mm_to_steps(&mtr_uv_mm, cnc_scale_uv());

					step_setPos(N_reg, &mtr_pt, &mtr_uv_pt); // only if no Run

					point_t enc_pt = fpoint_mm_to_steps(&mtr_mm, cnc_scale_enc());
					enc_setXY(enc_pt.x, enc_pt.y);
				}

				break;

			default:
				break;
			}
		}

		if (cmd.valid.flag.EoF) {
			cnc_applyMCmd();
//			rev ? pa_prev() : pa_next();
			if (rev)
				pa_prev();
			else
				pa_next();
		}
	}
		break;

	case ST_SEGMENT:
		if (cnc_rb())
			break; // fpga_mode is changed
		else if (step_ready() && (!fb_isEnabled() || hv_enabled)) {
			BOOL is_last[2] = {FALSE, FALSE}, valid[2] = {FALSE, FALSE};
			fpoint_t xy_mm = {0, 0}, uv_mm = {0, 0};
			point_t next = {0, 0}, uv_next = {0, 0};
			double t = 0, ts_xy = 0, ts_uv = 0;

			++step_id;

			if (fb_isEnabled())
				if (hv_enabled) {
					fpga_adcSnapshop();
					uint16_t adc = fpga_getADC(0);
					t = pid(adc, T, cnc_step());
				}
				else {
//					t = fb_lastT(T);
					t = T; // for test
				}
			else if (rb_ena)
				t = fb_Trb();
			else
				t = T;

			T_cur = t;

			if (line.valid) {
				xy_mm = line_getPoint(&line, step_id, &is_last[0], &valid[0]);
				ts_xy = line.step * t; // clocks
			}
			else if (arc.flag.valid) {
				xy_mm = arc_getPoint(&arc, step_id, &is_last[0], &valid[0]);
				ts_xy = arc.step_rad * arc.R * t;
			}
			else
				ts_xy = 0;

			if (uv_line.valid) {
				uv_mm = line_getPoint(&uv_line, step_id, &is_last[1], &valid[1]);
				ts_uv = uv_line.step * t;
			}
			else if (uv_arc.flag.valid) {
				uv_mm = arc_getPoint(&uv_arc, step_id, &is_last[1], &valid[1]);
				ts_uv = uv_arc.step_rad * arc.R * t;
			}
			else
				ts_uv = 0;

			if (pa_plane() == PLANE_XY && valid[0])
				next = fpoint_mm_to_steps(&xy_mm, cnc_scale_xy());
			else if (pa_plane() == PLANE_XYUV && valid[0] && valid[1]) {
				fpoint_t mtr_mm = uv_XY_to_motors(&xy_mm, &uv_mm);
				fpoint_t mtr_uv_mm = uv_UV_to_motors(&xy_mm, &uv_mm);

				next = fpoint_mm_to_steps(&mtr_mm, cnc_scale_xy());
				uv_next = fpoint_mm_to_steps(&mtr_uv_mm, cnc_scale_uv());

//				uv_next = uv_limit(uv_next);
			}
			else
				valid[0] = valid[1] = FALSE;

			int32_t nx = valid[0] ? next.x - mtr_pt.x : 0;
			int32_t ny = valid[0] ? next.y - mtr_pt.y : 0;
			int32_t nu = valid[1] ? uv_next.x - mtr_uv_pt.x : 0;
			int32_t nv = valid[1] ? uv_next.y - mtr_uv_pt.y : 0;

//			double ts_xy = ilength(nx, ny) * T;
//			double ts_uv = ilength(nu, nv) * T;
			double ts = ts_xy > ts_uv ? ts_xy : ts_uv;

			step_writeXYUV(N_reg, ts, nx, ny, nu, nv);

			mtr_pt.x += nx;
			mtr_pt.y += ny;
			mtr_uv_pt.x += nu;
			mtr_uv_pt.y += nv;

			if (rb_ena) {
				rb_path.x += abs(nx);
				rb_path.y += abs(ny);
			}

			switch (pa_plane()) {
			case PLANE_XY:
				if (!valid[0] || is_last[0])
					state = ST_WAIT;
				break;

			case PLANE_XYUV:
				if (!valid[0] || !valid[1] || (is_last[0] && is_last[1]))
					state = ST_WAIT;
				break;

			default:
					state = ST_WAIT;
			}

#ifdef PRINT
//			cnc_printState();
#endif
		}
		break;

	case ST_WAIT:
//				else if (step_ready())
		if (step_isStopped()) {
			if (cnc_rb())
				break;
			else
				state = ST_READ;
		}
		break;

	case ST_PAUSE:
		if (step_isStopped()) {
			if (T_pause) {
				step_writePause(N_reg, T_pause);
				BOOL run = !step_isStopped();
				double ms = fpga_pauseTicksToMs(T_pause);
				int ms_int = (int)round(ms);
				printf("PAUSE:%dms", ms_int);
				if (run)
					printf(" OK\n");
				else
					printf(" ERR\n");
			}
			else
				printf("PAUSE:0\n");

			state = ST_WAIT;
		}

		break;

	case ST_WAIT_BUTTON:
		if (step_ready()) {
			stop_req = TRUE;
			state = ST_READ;
		}
		break;

	case ST_END:
		if (step_isStopped())
			cnc_exit();
		break;

	case ST_ERROR:
		cnc_exit();
		break;

	default:
		state = ST_ERROR;
		break;
	}

	if (state != state_old) {
#ifdef PRINT
		switch (state) {
		case ST_IDLE: printf("ST_IDLE\n"); break;
		case ST_DIRECT: printf("ST_DIRECT\n"); break;
		case ST_CENTER: printf("ST_CENTER\n"); break;
		case ST_READ: printf("ST_READ\n"); break;
		case ST_SEGMENT: printf("ST_SEGMENT\n"); break;
		case ST_WAIT: printf("ST_WAIT\n"); break;
		case ST_PAUSE: printf("ST_PAUSE\n"); break;
		case ST_STOP: printf("ST_STOP\n"); break;
		case ST_WAIT_BUTTON: printf("ST_WAIT_BTN\n"); break;
		case ST_END: printf("ST_END\n"); break;
		case ST_ERROR: printf("ST_ERROR\n"); break;
		default: printf("state: %d\n", state); break;
		}
#endif

		// goto IDLE fpga_mode
		if (state == ST_IDLE) {
			gpo_setVoltageEnable(FALSE);
			gpo_setDrumEnable(FALSE);
			gpo_setPumpEnable(FALSE);
			gpo_apply();
		}
	}

	if (state >= ST_ERROR) {
#ifdef PRINT
		printf("ST_ERROR: RST\n");
#endif
		reset();
	}
}

// Execute M-command
BOOL m_changed = FALSE;
void cnc_applyMCmd() {
	if (m_changed) {
		m_changed = FALSE;
		gpo_apply();
	}
}

BOOL cnc_setMCmd(const gcmd_t* const cmd) {
	if (cmd->valid.flag.M && !rev) {
		m_changed = TRUE;

#ifdef PRINT
		printf("M%d", gcmd_M(cmd));
		if (cmd->valid.flag.P) {
			decimal_t P = float2fix( gcmd_P(cmd) );
			printf(" P%s%d.%03d", P.sign ? "-" : "", P.value, P.rem);
		}
		if (cmd->valid.flag.Q) {
			decimal_t Q = float2fix( gcmd_Q(cmd) );
			printf(" Q%s%d.%03d", Q.sign ? "-" : "", Q.value, Q.rem);
		}
		printf("\n");
#endif

		switch ( gcmd_M(cmd) ) {
		case 2: case 30:
			state = ST_END;
			step_write(0, 0, 0, TRUE, 0, 0);
#ifdef PRINT
			printf("END\n");
#endif
			break;

		case 0: // stop_req after frame finished
			stop_req = TRUE;
			break;

		case 1: // wait button start if enable mode
			if (alt_bp_btn_clicked()) {
//					cnc_turnOff();
				state = ST_WAIT_BUTTON;
			}
			break;

		case 40:
			cnc_par.flags.pump_ena = 1;
			gpo_setPumpEnable(TRUE);
			break;

		case 41:
			cnc_par.flags.pump_ena = 0;
			gpo_setPumpEnable(FALSE);
			break;

		case 82:
			cnc_par.flags.drum_ena = 1;
			gpo_setDrumEnable(TRUE);
			break;

		case 83:
			cnc_par.flags.drum_ena = 0;
			gpo_setDrumEnable(FALSE);
			break;

		case 84:
			cnc_par.flags.voltage_ena = 1;
			gpo_setVoltageEnable(TRUE);
			break;

		case 85:
			cnc_par.flags.voltage_ena = 0;
			gpo_setVoltageEnable(FALSE);
			break;

		case 101:
			fpga_setMotorOE(FALSE);
			break;

		case 102:
			if (cmd->valid.flag.P) {
				uint8_t p = double_to_uint8( gcmd_P(cmd) );
				cnc_par.pulse.width = p;
				gpo_setPulseWidth(p);
			}
			if (cmd->valid.flag.Q) {
				uint8_t q = double_to_uint8( gcmd_Q(cmd) );
				cnc_par.pulse.ratio = q;
				gpo_setPulseRatio(q);
			}
			break;

		case 103:
			if (cmd->valid.flag.P) {
				uint8_t p = double_to_uint8( gcmd_P(cmd) );
				cnc_par.current_index = p;
				gpo_setCurrentIndex(p);
			}
			break;

		case 104:
			if (cmd->valid.flag.P) {
				uint8_t p = double_to_uint8( gcmd_P(cmd) );
				cnc_par.voltage_level = p;
				gpo_setVoltageLevel(p);
			}
			break;

		case 105:
			if (cmd->valid.flag.P) {
				uint8_t p = double_to_uint8( gcmd_P(cmd) );
				cnc_par.drum_vel = p;
				gpo_setDrumVel(p);
			}
			break;

		case 106:
			if (cmd->valid.flag.P) {
				uv_setL( gcmd_P(cmd) );
			}
			if (cmd->valid.flag.Q) {
				uv_setH( gcmd_Q(cmd) );
			}
			break;

		case 107:
			if (cmd->valid.flag.P) {
				uv_setD( gcmd_P(cmd) );
				uv_applyParameters();
			}
			break;

		default:
			return FALSE;
		}

		step_writeTaskId(N_reg);
		return TRUE;
	}

	return FALSE;
}

void cnc_turnOff() {
	gpo_disableVDP();
	gpo_apply();
}

// todo: value can set manual, it only G-Code value
void cnc_turnOn() {
	gpo_setPumpEnable(cnc_par.flags.pump_ena);
	gpo_setDrumEnable(cnc_par.flags.drum_ena);
	gpo_setVoltageEnable(cnc_par.flags.voltage_ena);
	gpo_apply();
	// todo: pause
}

//static void init_string(const char* const str) {
//	cur_str = str;
//}

//static const char* get_string() { return cur_str; }

int cnc_getState() { return (int)state; }
BOOL cnc_isIdle() { return state == ST_IDLE; }

void cnc_runReq() {
	run_req = !cnc_isLocked() && (state == ST_IDLE || state == ST_STOP || state == ST_WAIT_BUTTON);
	stop_req = FALSE;

	if (run_req) {
		bkp_clearContextReg();
		printf("start_req\n");
	}
}

void cnc_stopReq() {
	stop_req = state != ST_IDLE && state != ST_STOP;
	run_req = FALSE;

	if (stop_req)
		printf("stop_req\n");
}

void cnc_abortReq() {
	cancel_req = state != ST_IDLE;

	if (cancel_req)
		printf("cancel_req\n");
}

void cnc_revReq() {
	rev_req = state == ST_STOP;
}

BOOL cnc_run() { return state > ST_IDLE && state < ST_ERROR; }
BOOL cnc_stop() { return state == ST_STOP; }
BOOL cnc_pause() { return state == ST_PAUSE; }
BOOL cnc_error() { return state >= ST_ERROR; }

// Rollback
int cnc_setRollbackLength(float value) {
	if (state == ST_IDLE) {
		rb_len = value;
		return 0;
	}
	else
		return -1;
}

float cnc_getRollbackLength() { return rb_len; }

void cnc_setRollbackAttempts(uint8_t value) {
	if (state == ST_IDLE)
		rb_attempts = value;
}

uint8_t cnc_getRollbackAttempt() { return rb_attempt; }
uint8_t cnc_getRollbackAttempts() { return rb_attempts; }

// clock / mm
float cnc_getT() { return T; }
// clock / mm
float cnc_getCurrentT() { return T_cur; }
// clock / mm
void cnc_setSpeedT(double value) {
	T = T_reg = value;
}

// mm / min
float cnc_getSpeed() { return (FPGA_CLOCK * 60.0) / T  ; }

// F - mm / min
void cnc_setSpeed(float F) {
	cnc_setSpeedT( speed_to_period(F) );
}

// mm / min
float cnc_speed() { return (FPGA_CLOCK * 60.0) / T  ; }

void cnc_printState() {
	fpga_snapMotor();
	int id = fpga_getTaskID();
	int fpga_x = fpga_getPos(0);
	int fpga_y = fpga_getPos(1);

	if (pa_plane() == PLANE_XY)
		printf("N:%d [%d, %d] H:%d [%d, %d] f:%x\n", (int)N_reg, (int)mtr_pt.x, (int)mtr_pt.y, id, fpga_x, fpga_y, flags);
	else {
		int fpga_u = fpga_getPos(2);
		int fpga_v = fpga_getPos(3);

		if (pa_plane() == PLANE_XYUV) {
			fpoint_t test, uv_test;

			test.x = uv_motor_to_X(mtr_pt.x, mtr_uv_pt.x);
			test.y = uv_motor_to_Y(mtr_pt.y, mtr_uv_pt.y);
			uv_test.x = uv_motor_to_U(mtr_pt.x, mtr_uv_pt.x);
			uv_test.y = uv_motor_to_V(mtr_pt.y, mtr_uv_pt.y);

			decimal_t X = float2fix(test.x);
			decimal_t Y = float2fix(test.y);
			decimal_t U = float2fix(uv_test.x);
			decimal_t V = float2fix(uv_test.y);

			printf("N:%d M[%d, %d, %d, %d] ([%s%d.%03d, %s%d.%03d, %s%d.%03d, %s%d.%03d]) H:%d [%d, %d, %d, %d] f:%x\n",
				(int)N_reg,
				(int)mtr_pt.x, (int)mtr_pt.y, (int)mtr_uv_pt.x, (int)mtr_uv_pt.y,
				X.sign ? "-" : "", X.value, X.rem,
				Y.sign ? "-" : "", Y.value, Y.rem,
				U.sign ? "-" : "", U.value, U.rem,
				V.sign ? "-" : "", V.value, V.rem,
				id,
				fpga_x, fpga_y, fpga_u, fpga_v,
				flags
			);
		}
		else
			printf("Soft: N=%d [%d, %d, %d, %d] Hard: N=%d [%d, %d, %d, %d] F=%x\n",
				(int)N_reg,
				(int)mtr_pt.x, (int)mtr_pt.y, (int)mtr_uv_pt.x, (int)mtr_uv_pt.y,
				id,
				fpga_x, fpga_y, fpga_u, fpga_v,
				flags
			);
	}
}

/////////////////////////////////////////////////////////////////////////////////////
void cnc_task_tb() {
#ifndef RB_TEST
#if TEST_NUM == 0
	const char* test[] = {
		"%",
		"G92 X0 Y0",
//		"G01 X0.003 Y0.003",
//		"G01 X0.003 Y-0.003",
//		"G01 X-0.003 Y-0.003",
		"G01 X-0.003 Y0.003",
		"M02",
		"%"
	};
#elif TEST_NUM == 1
	const char* test[] = {
		"%",
		"G92 X0 Y0",
//		"G01 X0.003 Y0.001",
//		"G01 X0.001 Y-0.003",
//		"G01 X-0.001 Y-0.003",
		"G01 X0.001 Y0.003",
		"M02",
		"%"
	};
#elif TEST_NUM == 2
	// square with 45 degrees sides
	const char* test[] = {
		"%",
		"G92 X0 Y0",
		"G01 X0.003 Y0.003",
		"G01 X0.006 Y0.000",
		"G01 X0.003 Y-0.003",
		"G01 X0.000 Y0.000",
		"M02",
		"%"
	};
#elif TEST_NUM == 3
	// square with 90 degrees sides
	const char* test[] = {
		"%",
		"G92 X0 Y0",
		"G01 Y0.003",
		"G01 X0.003",
		"G01 Y0.000",
		"G01 X0.000",
		"M02",
		"%"
	};
#elif TEST_NUM == 4
	// square
	const char* test[] = {
		"%",
		"G92 X0 Y0",
		"G1 X0.006 Y0.002 F0.060",
		"G1 X0.008 Y-0.004",
		"G1 X0.002 Y-0.006",
		"G1 X0 Y0",
		"M2",
		"%"
	};
#elif TEST_NUM == 5
	const char* test[] = {
		"%",
		"G92 X0 Y0",
		"G2 X0.010 Y-0.010 J-0.010",
		"G2 X0 Y-0.020 I-0.010",
		"G2 X-0.010 Y-0.010 J0.010",
		"G2 X0 Y0 I0.010",
		"M02",
		"%"
	};
#elif TEST_NUM == 6
	const char* test[] = {
		"%",
		"G92 X0 Y0",
		"G3 X-0.010 Y-0.010 J-0.010",
		"G3 X0 Y-0.020 I0.010",
		"G3 X0.010 Y-0.010 J0.010",
		"G3 X0 Y0 I-0.010",
		"M02",
		"%"
	};
#elif TEST_NUM == 7
	const char* test[] = {
		"%",
		"G92 X0 Y0",
		"G3 X0 Y0.020 J0.010",
		"G3 X0 Y0 J-0.010",
		"M02",
		"%"
	};
#elif TEST_NUM == 8
	const char* test[] = {
		"%",
		"G92 X0 Y0",
		"G2 X0 Y0.020 J0.010",
		"G2 X0 Y0 J-0.010",
		"M02",
		"%"
	};
#elif TEST_NUM == 9
	const char* test[] = {
		"%",
		"G92 X0 Y0",
		"G2 X0 Y0 J0.010",
		"M02",
		"%"
	};
#elif TEST_NUM == 10
	const char* test[] = {
		"%",
		"G92 X0 Y0",
		"M40", // pump ena
		"G4 P30000", // 1 sec
		"M105 P3 M82", // drum vel, drum ena
		"G1 X0.003 Y0.003",
//		"G01 X0.003 Y-0.003",
//		"G01 X-0.003 Y-0.003",
//		"G01 X-0.003 Y0.003",
		"M2",
		"%"
	};
#elif TEST_NUM == 11
	const char* test[] = {
		"%",
		"G92 X0 Y0",
		"G1 X0.010 Y0",
		"G2 X0.020 Y0.010 I0.010",
		"M02",
		"%"
	};
#elif TEST_NUM == 12
	const char* test[] = {
		"%",
		"M106 P150 Q50",
		"M107 P30",
		"G92 X0 Y0 U0 V0",
		"G2 X0.010 Y-0.010 J-0.010 G1 U0.010 V-0.010",
		"G2 X0 Y-0.020 I-0.010 G1 U0 V-0.020",
		"G2 X-0.010 Y-0.010 J0.010 G1 U-0.010 V-0.010",
		"G2 X0 Y0 I0.010 G1 U0 V0",
		"M02",
		"%"
	};
#else
	const char* test[] = {
		"G-code",
		"G92 X0 Y0",
		"G01 X0.3 Y0.1",
//		"G01 X0.04 Y-0.02",
//		"G01 X0.01 Y-0.03",
//		"G01 X0.000 Y0.000",
		"M02"
	};
#endif
#else
#if RB_TEST == 0
	const char* test[] = {
		"%",
		"G92 X0 Y0",
		"G1 X0.01 Y0.01",
		"G1 X0.02 Y0",
		"M2",
		"%"
	};

	cnc_setRollbackLength(0.015);
#endif
#endif

	step_setImitEna(TRUE);
	pa_clear();

#ifdef TEST_REVERSE
	rev_req = TRUE;
#endif

	pa_setBegin(0);

	for (int i = 0; i < sizeof(test)/sizeof(char*); i++)
		pa_write(test[i]);

	pa_print();

	fb_enable(FALSE);
	fpga_setInputLevel(0x300);
//	fpga_setInputLevel(0);
	cnc_runReq();
}

BOOL cnc_test_rev() {
	static arc_t arc;
	static line_t uv_line;
	const double step = 1;
	const fpoint_t A = {10,0}, B = {25,15}, C = {25,0};

	uv_setL( 150 );
	uv_setH( 50 );
	uv_setD( 30 );
	uv_applyParameters();

	arc_initCenter(&arc, &A, &B, &C, FALSE, step, cnc_scale_xy());
	line_init(&uv_line, &A, &B, FALSE, step);
	line_setStepRatio(&uv_line, arc_length(&arc));

	BOOL is_last, valid;
	size_t step_id = 3108;

	fpoint_t xy_mm = arc_getPoint(&arc, step_id, &is_last, &valid);
	fpoint_t uv_mm = line_getPoint(&uv_line, step_id, &is_last, &valid);

	fpoint_t mtr_mm = uv_XY_to_motors(&xy_mm, &uv_mm);
	fpoint_t mtr_uv_mm = uv_UV_to_motors(&xy_mm, &uv_mm);

	// Reverse
	// Read and init
	arc_initCenter(&arc, &A, &B, &C, FALSE, step, cnc_scale_xy());
	line_init(&uv_line, &A, &B, FALSE, step);
	line_setStepRatio(&uv_line, arc_length(&arc));

	arc_swap(&arc);
	line_swap(&uv_line);

	fpoint_t xy_mm2 = uv_motors_to_XY(&mtr_mm, &mtr_uv_mm);
	step_id = arc_getPos(&arc, &xy_mm2, cnc_scale_xy());

	fpoint_t rev_xy_mm = arc_getPoint(&arc, step_id, &is_last, &valid);
	fpoint_t rev_uv_mm = line_getPoint(&uv_line, step_id, &is_last, &valid);

	fpoint_t rev_mtr_mm = uv_XY_to_motors(&rev_xy_mm, &rev_uv_mm);
	fpoint_t rev_mtr_uv_mm = uv_UV_to_motors(&rev_xy_mm, &rev_uv_mm);

	return rev_mtr_mm.x == mtr_mm.x && rev_mtr_uv_mm.x == mtr_uv_mm.x;
}

int32_t ms_to_tick(double value) {
	value *= (FPGA_CLOCK / 1000);

	if (value >= (int32_t)INT32_MAX)
		return (int32_t)INT32_MAX;

	if (value <= (int32_t)INT32_MIN)
		return (int32_t)INT32_MIN;

	return (int32_t)value;
}

void cnc_test_direct() {
	fb_enable(FALSE);
	fpga_setSoftPermit(TRUE);
	fpga_setInputLevel(0x000);
	fpga_clearLimSwReg(0xFFFF);
	fpga_setMotorOE(TRUE); // Hold Off

	uint16_t res = fpga_getRun();
	printf("RUN:%04x\n", res);

	uint16_t limsw_reg = fpga_getLimSwReg();
	printf("limsw_reg:%02x\n", limsw_reg);

	while (limsw_reg) {
		fpga_clearLimSwReg(limsw_reg);
		limsw_reg = fpga_getLimSwReg();
		printf("limsw_reg:%02x\n", limsw_reg);
	}

	uint16_t adc_state = fpga_getADCStatus();
	printf("adc_state:%x\n", adc_state);

	param_reg[0] = 10;
	param_reg[1] = ms_to_tick(1000);
	param_reg[2] = 20;
	param_reg[3] = ms_to_tick(1000);
	param_reg[4] = 6;
	param_reg[5] = ms_to_tick(1000);
	param_reg[6] = 12;
	param_reg[7] = ms_to_tick(1000);

	direct_valid = 0xff;
	cnc_reqG1();

	res = fpga_getRun();
	printf("RUN:%04x\n", res);

	limsw_reg = fpga_getLimSwReg();
	printf("limsw_reg:%04x\n", limsw_reg);

	adc_state = fpga_getADCStatus();
	printf("adc_state:%x\n", adc_state);

	cnc_printState();
}

void cnc_direct_test_task() {
	static BOOL init = FALSE;
	static int i = 0;

	if (!init) {
		init = TRUE;
		fb_enable(FALSE);
		fpga_setInputLevel(0xFFFF);
		fpga_clearLimSwReg(0xFFFF);
		fpga_setSoftPermit(TRUE);
	}

	uint16_t res = fpga_getRun();
	printf("RUN:%04x\n", res);

	uint16_t limsw_reg = fpga_getLimSwReg();
	printf("limsw_reg:%04x\n", limsw_reg);

	uint16_t adc_state = fpga_getADCStatus();
	printf("adc_state:%x\n", adc_state);

	cnc_printState();

	memset(param_reg, 0, sizeof(param_reg));

	param_reg[2*i] = 1;
	param_reg[2*i + 1] = T_MIN;

	if (++i > 3)
		i = 0;

	direct_valid = 0xff;
	cnc_reqG1();
}
