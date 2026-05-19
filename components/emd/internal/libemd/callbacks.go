package libemd

/*
#include <stdlib.h>
#include <emd/agent_abi.h>
*/
import "C"
import (
	"runtime/cgo"
	"time"
	"unsafe"
)

// Event trampoline from C → Go.
// This function is called from the camera thread (C side) on every detection event.
//
//export goEventTrampoline
func goEventTrampoline(userCtx unsafe.Pointer, cEvt *C.emd_event_t) {
	if userCtx == nil || cEvt == nil {
		return
	}

	// Extract the Go channel from the handle
	h := *(*cgo.Handle)(userCtx)
	ch := h.Value().(chan<- Event)

	// Convert C event to Go event
	evt := Event{
		ID:          C.GoString(&cEvt.event_id[0]),
		CamID:       uint16(cEvt.cam_id),
		Type:        EventType(cEvt._type),
		Reason:      C.GoString(&cEvt.reason[0]),
		StartedPTS:  uint64(cEvt.started_pts_90khz),
		StartedTime: time.Now(),
		Codec:       uint8(cEvt.codec),
		FPS:         float64(cEvt.fps_estimate),
		CamName:     C.GoString(&cEvt.cam_name[0]),
		PreRollPTS:  uint64(cEvt.pre_roll_pts),
		PostRollPTS: uint64(cEvt.post_roll_pts),

		// Inspector signal snapshot
		ZScore:          float64(cEvt.z_score),
		IntraRatio:      float64(cEvt.intra_ratio),
		Bytes:           uint64(cEvt.byte_count),
		BPFSlow:         float64(cEvt.bpf_slow),
		BPFEwma:         float64(cEvt.bpf_ewma),
		BPFVar:          float64(cEvt.bpf_var),
		SinceKF:         uint32(cEvt.since_kf),
		FSMBefore:       uint8(cEvt.fsm_before),
		FSMAfter:        uint8(cEvt.fsm_after),
		TargetClassMask: uint8(cEvt.target_class_mask),
	}

	// Non-blocking send (drop if channel is full)
	select {
	case ch <- evt:
	default:
		// Event dropped - increment counter (TODO: add metrics)
	}
}

// Stats trampoline from C → Go.
// This function is called from the camera thread (C side) periodically.
//
//export goStatsTrampoline
func goStatsTrampoline(userCtx unsafe.Pointer, cSample *C.emd_stats_sample_t) {
	if userCtx == nil || cSample == nil {
		return
	}

	// Extract the Go channel from the handle
	h := *(*cgo.Handle)(userCtx)
	ch := h.Value().(chan<- StatsSample)

	// Convert C sample to Go sample
	sample := StatsSample{
		CamID:     uint16(cSample.cam_id),
		MonoNS:    uint64(cSample.mono_ns),
		BPFEwma:   float64(cSample.bpf_ewma),
		BPFSlow:   float64(cSample.bpf_slow),
		FSMState:  uint8(cSample.fsm_state),
		RTSPState: uint8(cSample.rtsp_state),
	}

	// Non-blocking send
	select {
	case ch <- sample:
	default:
		// Sample dropped
	}
}
