/*
 * RISC-V Core (FLPR) - Workload Simulation and MIPS Measurement
 * Communicates with ARM core via IPC
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/sys_clock.h>
#include <string.h>

/*
 * Use uptime in microseconds for timing measurements
 * The VPR timer runs at 1 MHz, not CPU frequency, so we need time-based measurements
 * RISC-V coprocessor frequency: 128 MHz (same as ARM Cortex-M33)
 */
#define RISCV_FREQ_MHZ 128

static inline uint64_t get_timestamp_us(void)
{
	return k_ticks_to_us_floor64(k_uptime_ticks());
}

#define STATS_INTERVAL_MS 1000
#define IPC_MSG_SIZE 64

/* IPC message types */
enum ipc_msg_type {
	IPC_MSG_STATS = 1,        /* RISC-V sends stats to ARM */
	IPC_MSG_SET_WORKLOAD = 2, /* ARM sets workload type */
	IPC_MSG_HEARTBEAT = 3,    /* Periodic heartbeat */
	IPC_MSG_AUDIO_DATA = 4,   /* RISC-V sends processed audio to ARM */
};

/* Workload types */
enum workload_type {
	WORKLOAD_IDLE = 0,
	WORKLOAD_MATRIX_MULT = 1,
	WORKLOAD_SORTING = 2,
	WORKLOAD_FFT_SIM = 3,
	WORKLOAD_CRYPTO_SIM = 4,
	WORKLOAD_MIXED = 5,
	WORKLOAD_AUDIO_PIPELINE = 6,
	WORKLOAD_AUDIO_PIPELINE_AEC = 7,
	WORKLOAD_PROXIMITY_VAD = 8,
	WORKLOAD_CHEST_RESONANCE = 9,
	WORKLOAD_CLOTHING_RUSTLE = 10,
	WORKLOAD_SPATIAL_NOISE_CANCEL = 11,
	WORKLOAD_WIND_NOISE_REDUCTION = 12,
	WORKLOAD_NECKLACE_FULL = 13,
};

struct ipc_message {
	uint8_t type;
	uint8_t workload;
	uint16_t reserved;
	uint32_t data[5];  /* Generic data payload - increased to fit stats_data (20 bytes) */
} __packed;

struct stats_data {
	uint64_t total_cycles;
	uint32_t iterations;
	uint32_t mips;
	uint32_t workload_type;
	uint32_t cpu_pct;  /* CPU utilization percentage */
} __packed;

static struct ipc_ept ep;
static enum workload_type current_workload = WORKLOAD_IDLE;
static uint64_t total_work_cycles = 0;
static uint32_t work_iterations = 0;

/* Volatile to prevent optimization */
static volatile uint32_t work_result = 0;

/*
 * Workload Simulations
 */

/* Matrix multiplication simulation (small 4x4 matrices) */
static uint64_t workload_matrix_mult(void)
{
	uint64_t start_us, end_us;
	int16_t a[4][4], b[4][4], c[4][4];

	/* Initialize matrices */
	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 4; j++) {
			a[i][j] = (i + j) & 0xFF;
			b[i][j] = (i * j) & 0xFF;
			c[i][j] = 0;
		}
	}

	start_us = get_timestamp_us();

	/* Matrix multiplication */
	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 4; j++) {
			for (int k = 0; k < 4; k++) {
				c[i][j] += a[i][k] * b[k][j];
			}
		}
	}

	end_us = get_timestamp_us();
	work_result = c[0][0];  /* Prevent optimization */

	/* Convert microseconds to CPU cycles (64 MHz = 64 cycles per microsecond) */
	return (end_us - start_us) * RISCV_FREQ_MHZ;
}

/* Sorting simulation (bubble sort) */
static uint64_t workload_sorting(void)
{
	uint64_t start_us, end_us;
	int32_t arr[32];

	/* Initialize array with pseudo-random values */
	for (int i = 0; i < 32; i++) {
		arr[i] = (i * 7 + 13) & 0xFFFF;
	}

	start_us = get_timestamp_us();

	/* Bubble sort */
	for (int i = 0; i < 31; i++) {
		for (int j = 0; j < 31 - i; j++) {
			if (arr[j] > arr[j + 1]) {
				int32_t temp = arr[j];
				arr[j] = arr[j + 1];
				arr[j + 1] = temp;
			}
		}
	}

	end_us = get_timestamp_us();
	work_result = arr[0];  /* Prevent optimization */

	return (end_us - start_us) * RISCV_FREQ_MHZ;
}

/* FFT simulation (butterfly operations) */
static uint64_t workload_fft_sim(void)
{
	uint64_t start_us, end_us;
	int32_t real[16], imag[16];

	/* Initialize with sample data */
	for (int i = 0; i < 16; i++) {
		real[i] = (i * 100) & 0xFFFF;
		imag[i] = 0;
	}

	start_us = get_timestamp_us();

	/* Simulate butterfly operations */
	for (int stage = 0; stage < 4; stage++) {
		for (int i = 0; i < 16; i += 2) {
			int32_t tr = real[i] + real[i + 1];
			int32_t ti = imag[i] + imag[i + 1];
			real[i + 1] = real[i] - real[i + 1];
			imag[i + 1] = imag[i] - imag[i + 1];
			real[i] = tr;
			imag[i] = ti;
		}
	}

	end_us = get_timestamp_us();
	work_result = real[0];  /* Prevent optimization */

	return (end_us - start_us) * RISCV_FREQ_MHZ;
}

/* Crypto simulation (simple AES-like operations) */
static uint64_t workload_crypto_sim(void)
{
	uint64_t start_us, end_us;
	uint8_t state[16];
	uint8_t key[16];

	/* Initialize state and key */
	for (int i = 0; i < 16; i++) {
		state[i] = i;
		key[i] = 15 - i;
	}

	start_us = get_timestamp_us();

	/* Simulate rounds of substitution and mixing */
	for (int round = 0; round < 4; round++) {
		/* SubBytes simulation */
		for (int i = 0; i < 16; i++) {
			state[i] = (state[i] ^ key[i]) + ((state[i] << 1) & 0xFF);
		}

		/* ShiftRows simulation */
		uint8_t temp = state[1];
		state[1] = state[5];
		state[5] = state[9];
		state[9] = state[13];
		state[13] = temp;

		/* MixColumns simulation */
		for (int i = 0; i < 4; i++) {
			uint8_t a = state[i * 4];
			uint8_t b = state[i * 4 + 1];
			state[i * 4] = a ^ b;
			state[i * 4 + 1] = b ^ a;
		}
	}

	end_us = get_timestamp_us();
	work_result = state[0];  /* Prevent optimization */

	return (end_us - start_us) * RISCV_FREQ_MHZ;
}

/*
 * Audio Processing Pipeline Simulation
 * Simulates: 3 mics @ 8kHz -> pre-processing -> beamforming -> post-processing -> VAD -> IPC transfer
 */
static uint64_t workload_audio_pipeline(void)
{
	uint64_t start_us, end_us;

	/* Simulate 3 microphone inputs at 8kHz (128 samples per frame = 16ms) */
	#define NUM_MICS 3
	#define FRAME_SIZE 128
	int16_t mic_data[NUM_MICS][FRAME_SIZE];
	int16_t filtered_data[NUM_MICS][FRAME_SIZE];
	int16_t beamformed_output[FRAME_SIZE];
	int16_t processed_output[FRAME_SIZE];

	start_us = get_timestamp_us();

	/* ===== 1. Simulate ADC reads from 3 microphones ===== */
	for (int mic = 0; mic < NUM_MICS; mic++) {
		for (int i = 0; i < FRAME_SIZE; i++) {
			/* Simulate reading from ADC with some variation per mic */
			mic_data[mic][i] = (i * (mic + 1) * 37 + work_result) & 0xFFF;
		}
	}

	/* ===== 2. Pre-processing: DC removal and noise filtering ===== */
	for (int mic = 0; mic < NUM_MICS; mic++) {
		int32_t dc_sum = 0;

		/* Calculate DC offset */
		for (int i = 0; i < FRAME_SIZE; i++) {
			dc_sum += mic_data[mic][i];
		}
		int16_t dc_offset = dc_sum / FRAME_SIZE;

		/* Remove DC and apply simple FIR filter */
		for (int i = 2; i < FRAME_SIZE; i++) {
			int32_t filtered = mic_data[mic][i] - dc_offset;
			/* 3-tap FIR filter: y[n] = 0.25*x[n-2] + 0.5*x[n-1] + 0.25*x[n] */
			filtered = (mic_data[mic][i-2] + 2*mic_data[mic][i-1] + mic_data[mic][i]) / 4;
			filtered_data[mic][i] = filtered - dc_offset;
		}
	}

	/* ===== 3. Beamforming: Delay-and-sum with weights ===== */
	/* Simulate spatial filtering to enhance signal from target direction */
	for (int i = 0; i < FRAME_SIZE; i++) {
		int32_t sum = 0;

		/* Delay-and-sum beamforming with weights */
		/* Mic 0: center (weight 0.5, no delay) */
		/* Mic 1: left (weight 0.25, delay 2 samples) */
		/* Mic 2: right (weight 0.25, delay 2 samples) */

		int delay_mic1 = (i >= 2) ? i - 2 : 0;
		int delay_mic2 = (i >= 2) ? i - 2 : 0;

		sum += (filtered_data[0][i] * 2);      /* Center mic, weight 0.5 */
		sum += filtered_data[1][delay_mic1];   /* Left mic, weight 0.25 */
		sum += filtered_data[2][delay_mic2];   /* Right mic, weight 0.25 */

		beamformed_output[i] = sum / 4;
	}

	/* ===== 4. Post-processing: Noise suppression and AGC ===== */
	/* Simulate spectral subtraction for noise reduction */
	int32_t signal_energy = 0;
	int32_t noise_floor = 100;  /* Estimated noise floor */

	for (int i = 0; i < FRAME_SIZE; i++) {
		int32_t sample = beamformed_output[i];
		int32_t magnitude = (sample < 0) ? -sample : sample;

		/* Noise suppression: subtract noise floor */
		if (magnitude > noise_floor) {
			processed_output[i] = sample;
		} else {
			processed_output[i] = 0;
		}

		signal_energy += (processed_output[i] * processed_output[i]);
	}

	/* Apply Automatic Gain Control (AGC) */
	int32_t rms = signal_energy / FRAME_SIZE;
	int16_t gain = 1;
	if (rms > 0) {
		/* Target RMS level: 2000, scale gain accordingly */
		gain = (2000 * 256) / (rms + 1);  /* Fixed-point math */
		if (gain > 512) gain = 512;  /* Limit max gain to 2x */
		if (gain < 64) gain = 64;    /* Limit min gain to 0.25x */
	}

	for (int i = 0; i < FRAME_SIZE; i++) {
		processed_output[i] = (processed_output[i] * gain) / 256;
	}

	/* ===== 5. Voice Activity Detection (VAD) ===== */
	/* Simple energy-based VAD with zero-crossing rate */
	int32_t frame_energy = 0;
	int32_t zero_crossings = 0;

	for (int i = 0; i < FRAME_SIZE; i++) {
		frame_energy += (processed_output[i] * processed_output[i]);

		if (i > 0) {
			/* Count zero crossings */
			if ((processed_output[i] >= 0 && processed_output[i-1] < 0) ||
			    (processed_output[i] < 0 && processed_output[i-1] >= 0)) {
				zero_crossings++;
			}
		}
	}

	frame_energy /= FRAME_SIZE;

	/* VAD decision: voice present if high energy and moderate zero-crossing rate */
	bool voice_detected = (frame_energy > 1000) && (zero_crossings > 10) && (zero_crossings < 80);

	end_us = get_timestamp_us();

	/* ===== 6. Transfer to ARM core via IPC ===== */
	/* Only send if voice is detected to save bandwidth */
	if (voice_detected) {
		#if DT_NODE_EXISTS(DT_NODELABEL(ipc0))
		struct ipc_message msg;
		memset(&msg, 0, sizeof(msg));
		msg.type = IPC_MSG_AUDIO_DATA;
		msg.workload = WORKLOAD_AUDIO_PIPELINE;

		/* Pack first 16 samples as a proof-of-concept (4 words * 4 bytes = 16 bytes = 8 samples) */
		/* In real implementation, would use larger IPC buffers or streaming */
		msg.data[0] = (processed_output[0] & 0xFFFF) | ((processed_output[1] & 0xFFFF) << 16);
		msg.data[1] = (processed_output[2] & 0xFFFF) | ((processed_output[3] & 0xFFFF) << 16);
		msg.data[2] = frame_energy;  /* Include VAD metrics */
		msg.data[3] = zero_crossings;

		int ret = ipc_service_send(&ep, &msg, sizeof(msg));
		if (ret < 0) {
			/* IPC send failed, but don't count as error in workload */
		}
		#endif

		work_result = processed_output[0];  /* Prevent optimization */
	} else {
		work_result = 0;  /* No voice detected */
	}

	return (end_us - start_us) * RISCV_FREQ_MHZ;
}

/* Audio Pipeline with Acoustic Echo Cancellation (AEC) */
static uint64_t workload_audio_pipeline_aec(void)
{
	uint64_t start_us, end_us;

	/* Simulate 3 microphone inputs at 8kHz (128 samples per frame = 16ms) */
	#define NUM_MICS 3
	#define FRAME_SIZE 128
	#define AEC_FILTER_TAPS 256  /* 256-tap filter for 30ms echo tail @ 8kHz */

	int16_t mic_data[NUM_MICS][FRAME_SIZE];
	int16_t filtered_data[NUM_MICS][FRAME_SIZE];
	int16_t beamformed_output[FRAME_SIZE];
	int16_t processed_output[FRAME_SIZE];

	/* AEC-specific buffers */
	static int16_t aec_filter[AEC_FILTER_TAPS];  /* Adaptive filter coefficients */
	static int16_t far_end_buffer[AEC_FILTER_TAPS];  /* Reference signal (speaker output) */
	int16_t echo_estimate[FRAME_SIZE];
	int16_t error_signal[FRAME_SIZE];

	start_us = get_timestamp_us();

	/* ===== STAGES 1-5: Full Audio Pipeline (same as workload 6) ===== */

	/* 1. Simulate ADC reads from 3 microphones */
	for (int mic = 0; mic < NUM_MICS; mic++) {
		for (int i = 0; i < FRAME_SIZE; i++) {
			mic_data[mic][i] = (i * (mic + 1) * 37 + work_result) & 0xFFF;
		}
	}

	/* 2. Pre-processing: DC removal and noise filtering */
	for (int mic = 0; mic < NUM_MICS; mic++) {
		int32_t dc_sum = 0;

		for (int i = 0; i < FRAME_SIZE; i++) {
			dc_sum += mic_data[mic][i];
		}
		int16_t dc_offset = dc_sum / FRAME_SIZE;

		for (int i = 2; i < FRAME_SIZE; i++) {
			int32_t filtered = mic_data[mic][i] - dc_offset;
			filtered = (mic_data[mic][i-2] + 2*mic_data[mic][i-1] + mic_data[mic][i]) / 4;
			filtered_data[mic][i] = filtered - dc_offset;
		}
	}

	/* 3. Beamforming: Delay-and-sum with weights */
	for (int i = 0; i < FRAME_SIZE; i++) {
		int32_t sum = 0;

		int delay_mic1 = (i >= 2) ? i - 2 : 0;
		int delay_mic2 = (i >= 2) ? i - 2 : 0;

		sum += (filtered_data[0][i] * 2);
		sum += filtered_data[1][delay_mic1];
		sum += filtered_data[2][delay_mic2];

		beamformed_output[i] = sum / 4;
	}

	/* 4. Post-processing: Noise suppression and AGC */
	int32_t signal_energy = 0;
	int32_t noise_floor = 100;

	for (int i = 0; i < FRAME_SIZE; i++) {
		int32_t sample = beamformed_output[i];
		int32_t magnitude = (sample < 0) ? -sample : sample;

		if (magnitude > noise_floor) {
			processed_output[i] = sample;
		} else {
			processed_output[i] = 0;
		}

		signal_energy += (processed_output[i] * processed_output[i]);
	}

	/* Apply AGC */
	int32_t rms = signal_energy / FRAME_SIZE;
	int16_t gain = 1;
	if (rms > 0) {
		gain = (2000 * 256) / (rms + 1);
		if (gain > 512) gain = 512;
		if (gain < 64) gain = 64;
	}

	for (int i = 0; i < FRAME_SIZE; i++) {
		processed_output[i] = (processed_output[i] * gain) / 256;
	}

	/* 5. Voice Activity Detection (VAD) */
	int32_t frame_energy = 0;
	int32_t zero_crossings = 0;

	for (int i = 0; i < FRAME_SIZE; i++) {
		frame_energy += (processed_output[i] * processed_output[i]);

		if (i > 0) {
			if ((processed_output[i] >= 0 && processed_output[i-1] < 0) ||
			    (processed_output[i] < 0 && processed_output[i-1] >= 0)) {
				zero_crossings++;
			}
		}
	}

	frame_energy /= FRAME_SIZE;
	bool voice_detected = (frame_energy > 1000) && (zero_crossings > 10) && (zero_crossings < 80);

	/* ===== STAGE 6: ACOUSTIC ECHO CANCELLATION ===== */

	/* Simulate far-end signal (speaker output that creates echo) */
	for (int i = 0; i < FRAME_SIZE && i < AEC_FILTER_TAPS; i++) {
		far_end_buffer[i] = (i * 29 + work_result) & 0x7FF;  /* Simulated reference signal */
	}

	/* AEC: Adaptive NLMS (Normalized Least Mean Squares) Filter */
	/* Update every 2nd sample to reduce computational cost */
	for (int n = 0; n < FRAME_SIZE; n++) {
		int32_t echo_est = 0;

		/* Convolution: estimate echo from far-end reference */
		for (int k = 0; k < AEC_FILTER_TAPS && k <= n; k++) {
			echo_est += (aec_filter[k] * far_end_buffer[n - k]) / 256;
		}
		echo_estimate[n] = echo_est;

		/* Calculate error signal (near-end - echo_estimate) */
		error_signal[n] = processed_output[n] - echo_estimate[n];

		/* Adaptive filter update (every 2nd sample) */
		if (n % 2 == 0) {
			/* Calculate normalization factor */
			int32_t power = 0;
			for (int k = 0; k < AEC_FILTER_TAPS && k <= n; k++) {
				int32_t val = far_end_buffer[n - k];
				power += (val * val) / 256;
			}
			power = power / AEC_FILTER_TAPS + 1;  /* Prevent division by zero */

			/* NLMS update: w[k] = w[k] + (mu * error * x[k]) / power */
			int16_t mu = 16;  /* Step size (fixed-point: 16/256 = 0.0625) */
			int32_t update_factor = (mu * error_signal[n]) / power;

			for (int k = 0; k < AEC_FILTER_TAPS && k <= n; k++) {
				int32_t update = (update_factor * far_end_buffer[n - k]) / 256;
				aec_filter[k] += update;

				/* Limit coefficient range to prevent overflow */
				if (aec_filter[k] > 8192) aec_filter[k] = 8192;
				if (aec_filter[k] < -8192) aec_filter[k] = -8192;
			}
		}
	}

	/* Double-talk detection: Check if near-end and far-end both have energy */
	int32_t near_end_energy = frame_energy;
	int32_t far_end_energy = 0;
	for (int i = 0; i < FRAME_SIZE; i++) {
		far_end_energy += (far_end_buffer[i] * far_end_buffer[i]);
	}
	far_end_energy /= FRAME_SIZE;

	/* If double-talk detected, freeze filter adaptation */
	bool double_talk = (near_end_energy > 500) && (far_end_energy > 500);

	/* Residual echo suppression: spectral subtraction on error signal */
	int16_t final_output[FRAME_SIZE];
	for (int i = 0; i < FRAME_SIZE; i++) {
		if (double_talk) {
			/* During double-talk, pass through with minimal processing */
			final_output[i] = processed_output[i];
		} else {
			/* Apply residual echo suppression */
			int32_t suppressed = error_signal[i];
			int32_t magnitude = (suppressed < 0) ? -suppressed : suppressed;

			/* Suppress residual echo below threshold */
			if (magnitude < 50) {
				suppressed = suppressed / 2;  /* Attenuate low-level residuals */
			}
			final_output[i] = suppressed;
		}
	}

	end_us = get_timestamp_us();

	/* ===== 7. Transfer to ARM core via IPC ===== */
	if (voice_detected) {
		#if DT_NODE_EXISTS(DT_NODELABEL(ipc0))
		struct ipc_message msg;
		memset(&msg, 0, sizeof(msg));
		msg.type = IPC_MSG_AUDIO_DATA;
		msg.workload = WORKLOAD_AUDIO_PIPELINE_AEC;

		msg.data[0] = (final_output[0] & 0xFFFF) | ((final_output[1] & 0xFFFF) << 16);
		msg.data[1] = (final_output[2] & 0xFFFF) | ((final_output[3] & 0xFFFF) << 16);
		msg.data[2] = frame_energy;
		msg.data[3] = zero_crossings;
		msg.data[4] = double_talk ? 1 : 0;  /* Double-talk flag */

		int ret = ipc_service_send(&ep, &msg, sizeof(msg));
		if (ret < 0) {
			/* IPC send failed */
		}
		#endif

		work_result = final_output[0];
	} else {
		work_result = 0;
	}

	return (end_us - start_us) * RISCV_FREQ_MHZ;
}

/* Proximity-Based VAD - Distinguish wearer from far-field speakers */
static uint64_t workload_proximity_vad(void)
{
	uint64_t start_us, end_us;

	#define NUM_MICS 3
	#define FRAME_SIZE 128
	int16_t mic_data[NUM_MICS][FRAME_SIZE];

	start_us = get_timestamp_us();

	/* Simulate ADC reads from 3 microphones */
	for (int mic = 0; mic < NUM_MICS; mic++) {
		for (int i = 0; i < FRAME_SIZE; i++) {
			mic_data[mic][i] = (i * (mic + 1) * 37 + work_result) & 0xFFF;
		}
	}

	/* Calculate energy per microphone */
	int32_t mic_energy[NUM_MICS];
	for (int mic = 0; mic < NUM_MICS; mic++) {
		int32_t energy = 0;
		for (int i = 0; i < FRAME_SIZE; i++) {
			int32_t sample = mic_data[mic][i];
			energy += (sample * sample) / 256;
		}
		mic_energy[mic] = energy / FRAME_SIZE;
	}

	/* Near-field detection: Large energy differences between mics */
	/* Far-field: Similar energy levels across mics */
	int32_t energy_diff = 0;
	int32_t energy_avg = 0;
	for (int mic = 0; mic < NUM_MICS; mic++) {
		energy_avg += mic_energy[mic];
	}
	energy_avg /= NUM_MICS;

	for (int mic = 0; mic < NUM_MICS; mic++) {
		int32_t diff = mic_energy[mic] - energy_avg;
		if (diff < 0) diff = -diff;
		energy_diff += diff;
	}

	/* Calculate ratio: high ratio = near-field (wearer) */
	int32_t proximity_ratio = (energy_diff * 100) / (energy_avg + 1);

	/* Spectral analysis for human voice (85-255 Hz fundamental) */
	int32_t voice_band_energy = 0;
	int32_t noise_band_energy = 0;

	/* Simple spectral estimation using zero-crossings and energy distribution */
	for (int mic = 0; mic < NUM_MICS; mic++) {
		int zero_crossings = 0;
		int32_t low_freq_energy = 0;
		int32_t high_freq_energy = 0;

		for (int i = 1; i < FRAME_SIZE; i++) {
			if ((mic_data[mic][i] >= 0 && mic_data[mic][i-1] < 0) ||
			    (mic_data[mic][i] < 0 && mic_data[mic][i-1] >= 0)) {
				zero_crossings++;
			}

			/* Rough frequency separation based on sample position */
			if (i < FRAME_SIZE / 4) {
				low_freq_energy += (mic_data[mic][i] * mic_data[mic][i]) / 256;
			} else {
				high_freq_energy += (mic_data[mic][i] * mic_data[mic][i]) / 256;
			}
		}

		/* Voice typically has 10-30 zero crossings per 16ms frame @ 8kHz */
		if (zero_crossings >= 10 && zero_crossings <= 30) {
			voice_band_energy += low_freq_energy;
		} else {
			noise_band_energy += high_freq_energy;
		}
	}

	/* VAD decision: near-field + voice characteristics */
	bool is_wearer_voice = (proximity_ratio > 30) &&  /* Near-field */
	                       (voice_band_energy > (noise_band_energy * 2)) &&  /* Voice-like */
	                       (energy_avg > 500);  /* Minimum energy threshold */

	end_us = get_timestamp_us();

	work_result = is_wearer_voice ? 1 : 0;
	return (end_us - start_us) * RISCV_FREQ_MHZ;
}

/* Chest Resonance Detection - Detect low-frequency resonance from chest cavity */
static uint64_t workload_chest_resonance(void)
{
	uint64_t start_us, end_us;

	#define NUM_MICS 3
	#define FRAME_SIZE 128
	int16_t mic_data[NUM_MICS][FRAME_SIZE];

	start_us = get_timestamp_us();

	/* Simulate ADC reads */
	for (int mic = 0; mic < NUM_MICS; mic++) {
		for (int i = 0; i < FRAME_SIZE; i++) {
			mic_data[mic][i] = (i * (mic + 1) * 41 + work_result) & 0xFFF;
		}
	}

	/* Analyze 50-200 Hz energy (chest resonance band) */
	/* At 8kHz sampling, this corresponds to specific zero-crossing rates */
	int32_t resonance_energy[NUM_MICS];
	int32_t resonance_coherence = 0;

	for (int mic = 0; mic < NUM_MICS; mic++) {
		/* Extract low-frequency component (chest resonance) */
		int32_t low_freq_sum = 0;
		int32_t low_freq_samples = 0;

		/* Simple low-pass filter to isolate 50-200 Hz */
		/* 8 kHz / 4 = 2 kHz cutoff (rough approximation) */
		for (int i = 4; i < FRAME_SIZE; i += 4) {
			/* Downsample by 4 to focus on low frequencies */
			int32_t avg = (mic_data[mic][i-3] + mic_data[mic][i-2] +
			               mic_data[mic][i-1] + mic_data[mic][i]) / 4;
			low_freq_sum += (avg * avg) / 256;
			low_freq_samples++;
		}

		resonance_energy[mic] = low_freq_sum / low_freq_samples;
	}

	/* Calculate coherence across microphones */
	/* Chest resonance should be coherent across all mics when speaking */
	int32_t energy_variance = 0;
	int32_t energy_avg = 0;
	for (int mic = 0; mic < NUM_MICS; mic++) {
		energy_avg += resonance_energy[mic];
	}
	energy_avg /= NUM_MICS;

	for (int mic = 0; mic < NUM_MICS; mic++) {
		int32_t diff = resonance_energy[mic] - energy_avg;
		energy_variance += (diff * diff) / 256;
	}
	energy_variance /= NUM_MICS;

	/* High energy + low variance = coherent chest resonance */
	int32_t coherence_score = (energy_avg * 100) / (energy_variance + 1);

	/* Detect chest resonance pattern */
	bool chest_resonance_detected = (energy_avg > 300) &&  /* Minimum energy */
	                                 (coherence_score > 50);  /* High coherence */

	end_us = get_timestamp_us();

	work_result = chest_resonance_detected ? energy_avg : 0;
	return (end_us - start_us) * RISCV_FREQ_MHZ;
}

/* Clothing Rustle Suppression - Detect and suppress impulse noise from clothing */
static uint64_t workload_clothing_rustle(void)
{
	uint64_t start_us, end_us;

	#define NUM_MICS 3
	#define FRAME_SIZE 128
	int16_t mic_data[NUM_MICS][FRAME_SIZE];
	int16_t processed_output[FRAME_SIZE];

	start_us = get_timestamp_us();

	/* Simulate ADC reads */
	for (int mic = 0; mic < NUM_MICS; mic++) {
		for (int i = 0; i < FRAME_SIZE; i++) {
			mic_data[mic][i] = (i * (mic + 1) * 43 + work_result) & 0xFFF;
		}
	}

	/* Detect impulse noise characteristics of clothing rustle:
	 * - High-frequency transients
	 * - Short duration spikes
	 * - Uncorrelated between microphones (localized contact) */

	bool rustle_detected[FRAME_SIZE] = {false};

	/* Cross-correlation analysis between mics */
	for (int i = 2; i < FRAME_SIZE; i++) {
		/* Calculate instantaneous energy change */
		int32_t energy_change[NUM_MICS];
		int32_t total_change = 0;
		int32_t correlation = 0;

		for (int mic = 0; mic < NUM_MICS; mic++) {
			/* Second derivative for impulse detection */
			int32_t accel = mic_data[mic][i] - 2*mic_data[mic][i-1] + mic_data[mic][i-2];
			if (accel < 0) accel = -accel;
			energy_change[mic] = accel;
			total_change += accel;
		}

		/* Check correlation */
		int32_t change_avg = total_change / NUM_MICS;
		for (int mic = 0; mic < NUM_MICS; mic++) {
			int32_t diff = energy_change[mic] - change_avg;
			if (diff < 0) diff = -diff;
			correlation += diff;
		}

		/* High energy change + low correlation = clothing rustle */
		if ((total_change > 500) && (correlation > 300)) {
			rustle_detected[i] = true;
			/* Mark surrounding samples too */
			if (i > 0) rustle_detected[i-1] = true;
			if (i < FRAME_SIZE - 1) rustle_detected[i+1] = true;
		}
	}

	/* Apply suppression */
	int rustles_suppressed = 0;
	for (int i = 0; i < FRAME_SIZE; i++) {
		if (rustle_detected[i]) {
			/* Attenuate impulse noise by 75% */
			processed_output[i] = mic_data[0][i] / 4;
			rustles_suppressed++;
		} else {
			processed_output[i] = mic_data[0][i];
		}
	}

	end_us = get_timestamp_us();

	work_result = rustles_suppressed;
	return (end_us - start_us) * RISCV_FREQ_MHZ;
}

/* Spatial Noise Cancellation - Use mic geometry to cancel ambient noise */
static uint64_t workload_spatial_noise_cancel(void)
{
	uint64_t start_us, end_us;

	#define NUM_MICS 3
	#define FRAME_SIZE 128
	int16_t mic_data[NUM_MICS][FRAME_SIZE];
	int16_t noise_estimate[FRAME_SIZE];
	int16_t clean_output[FRAME_SIZE];

	start_us = get_timestamp_us();

	/* Simulate ADC reads */
	for (int mic = 0; mic < NUM_MICS; mic++) {
		for (int i = 0; i < FRAME_SIZE; i++) {
			mic_data[mic][i] = (i * (mic + 1) * 47 + work_result) & 0xFFF;
		}
	}

	/* Spatial noise cancellation using Generalized Sidelobe Canceller (GSC) approach:
	 * 1. Beamform to focus on wearer (primary path)
	 * 2. Create null beam for noise reference (blocking matrix)
	 * 3. Adaptive filter to estimate and cancel noise */

	/* Primary beam: focus on wearer (downward/forward direction) */
	for (int i = 0; i < FRAME_SIZE; i++) {
		int32_t beamformed = 0;
		/* Weight center mic higher (closer to mouth) */
		beamformed = mic_data[0][i] * 2 + mic_data[1][i] + mic_data[2][i];
		clean_output[i] = beamformed / 4;
	}

	/* Noise reference: create null in wearer direction */
	for (int i = 0; i < FRAME_SIZE; i++) {
		/* Subtract center mic to create null */
		int32_t noise_ref = (mic_data[1][i] + mic_data[2][i]) / 2 - mic_data[0][i];
		noise_estimate[i] = noise_ref;
	}

	/* Adaptive noise cancellation (LMS-like) */
	static int16_t noise_filter[32] = {0};  /* Simple 32-tap filter */

	for (int n = 32; n < FRAME_SIZE; n++) {
		/* Estimate noise component in primary beam */
		int32_t noise_est = 0;
		for (int k = 0; k < 32; k++) {
			noise_est += (noise_filter[k] * noise_estimate[n - k]) / 256;
		}

		/* Subtract noise estimate */
		int32_t error = clean_output[n] - noise_est;
		clean_output[n] = error;

		/* Adapt filter (simple LMS) */
		int16_t mu = 8;  /* Step size */
		for (int k = 0; k < 32; k++) {
			int32_t update = (mu * error * noise_estimate[n - k]) / (FRAME_SIZE * 256);
			noise_filter[k] += update;

			/* Limit coefficients */
			if (noise_filter[k] > 2048) noise_filter[k] = 2048;
			if (noise_filter[k] < -2048) noise_filter[k] = -2048;
		}
	}

	/* Calculate noise reduction achieved */
	int32_t output_energy = 0;
	int32_t noise_energy = 0;
	for (int i = 0; i < FRAME_SIZE; i++) {
		output_energy += (clean_output[i] * clean_output[i]) / 256;
		noise_energy += (noise_estimate[i] * noise_estimate[i]) / 256;
	}

	end_us = get_timestamp_us();

	work_result = output_energy / FRAME_SIZE;
	return (end_us - start_us) * RISCV_FREQ_MHZ;
}

/* Wind Noise Reduction - Detect and suppress wind noise */
static uint64_t workload_wind_noise_reduction(void)
{
	uint64_t start_us, end_us;

	#define NUM_MICS 3
	#define FRAME_SIZE 128
	int16_t mic_data[NUM_MICS][FRAME_SIZE];
	int16_t processed_output[FRAME_SIZE];

	start_us = get_timestamp_us();

	/* Simulate ADC reads */
	for (int mic = 0; mic < NUM_MICS; mic++) {
		for (int i = 0; i < FRAME_SIZE; i++) {
			mic_data[mic][i] = (i * (mic + 1) * 51 + work_result) & 0xFFF;
		}
	}

	/* Wind noise characteristics:
	 * - Low frequency (< 500 Hz dominant)
	 * - Low correlation between microphones
	 * - Temporal characteristics (gusts) */

	/* Calculate per-mic low-frequency energy and correlation */
	int32_t low_freq_energy[NUM_MICS];
	for (int mic = 0; mic < NUM_MICS; mic++) {
		int32_t energy = 0;
		/* Focus on low frequencies */
		for (int i = 8; i < FRAME_SIZE; i += 8) {
			/* Decimate by 8 to focus on < 1kHz */
			int32_t sample = mic_data[mic][i];
			energy += (sample * sample) / 256;
		}
		low_freq_energy[mic] = energy / (FRAME_SIZE / 8);
	}

	/* Calculate inter-mic correlation */
	int32_t correlation = 0;
	for (int i = 0; i < FRAME_SIZE; i++) {
		int32_t cross_product = 0;
		/* Correlation between mic pairs */
		cross_product += mic_data[0][i] * mic_data[1][i];
		cross_product += mic_data[1][i] * mic_data[2][i];
		cross_product += mic_data[0][i] * mic_data[2][i];
		correlation += cross_product / (256 * 3);
	}
	correlation = correlation / FRAME_SIZE;

	/* Wind detection: high low-freq energy + low correlation */
	int32_t avg_energy = (low_freq_energy[0] + low_freq_energy[1] + low_freq_energy[2]) / 3;
	bool wind_detected = (avg_energy > 400) && (correlation < 100);

	/* Wind suppression strategy */
	if (wind_detected) {
		/* Use microphone with lowest wind energy */
		int min_energy_mic = 0;
		for (int mic = 1; mic < NUM_MICS; mic++) {
			if (low_freq_energy[mic] < low_freq_energy[min_energy_mic]) {
				min_energy_mic = mic;
			}
		}

		/* Use best mic and apply spectral subtraction for low frequencies */
		for (int i = 0; i < FRAME_SIZE; i++) {
			int32_t sample = mic_data[min_energy_mic][i];

			/* High-pass filter to attenuate wind frequencies */
			if (i >= 2) {
				/* Simple high-pass: y[n] = x[n] - x[n-1] */
				sample = mic_data[min_energy_mic][i] - mic_data[min_energy_mic][i-1];
			}

			processed_output[i] = sample;
		}
	} else {
		/* No wind: use normal beamformed output */
		for (int i = 0; i < FRAME_SIZE; i++) {
			processed_output[i] = (mic_data[0][i] * 2 + mic_data[1][i] + mic_data[2][i]) / 4;
		}
	}

	end_us = get_timestamp_us();

	work_result = wind_detected ? 1 : 0;
	return (end_us - start_us) * RISCV_FREQ_MHZ;
}

/* Full Necklace Pipeline - Complete audio processing for necklace form factor */
static uint64_t workload_necklace_full(void)
{
	uint64_t start_us, end_us;

	#define NUM_MICS 3
	#define FRAME_SIZE 128
	int16_t mic_data[NUM_MICS][FRAME_SIZE];
	int16_t stage1_output[NUM_MICS][FRAME_SIZE];  /* After DC removal */
	int16_t stage2_output[FRAME_SIZE];            /* After spatial noise cancel */
	int16_t stage3_output[FRAME_SIZE];            /* After wind reduction */
	int16_t stage4_output[FRAME_SIZE];            /* After clothing rustle suppression */
	int16_t stage5_output[FRAME_SIZE];            /* After beamforming */
	int16_t final_output[FRAME_SIZE];             /* After AGC */

	start_us = get_timestamp_us();

	/* ===== STAGE 1: ADC + DC Removal ===== */
	for (int mic = 0; mic < NUM_MICS; mic++) {
		/* Simulate ADC */
		for (int i = 0; i < FRAME_SIZE; i++) {
			mic_data[mic][i] = (i * (mic + 1) * 53 + work_result) & 0xFFF;
		}

		/* DC removal */
		int32_t dc_sum = 0;
		for (int i = 0; i < FRAME_SIZE; i++) {
			dc_sum += mic_data[mic][i];
		}
		int16_t dc_offset = dc_sum / FRAME_SIZE;

		for (int i = 0; i < FRAME_SIZE; i++) {
			stage1_output[mic][i] = mic_data[mic][i] - dc_offset;
		}
	}

	/* ===== STAGE 2: Spatial Noise Cancellation ===== */
	/* Primary beam + noise reference */
	for (int i = 0; i < FRAME_SIZE; i++) {
		int32_t primary = (stage1_output[0][i] * 2 + stage1_output[1][i] + stage1_output[2][i]) / 4;
		int32_t noise_ref = (stage1_output[1][i] + stage1_output[2][i]) / 2 - stage1_output[0][i];
		/* Simple noise subtraction (full adaptive filter in real implementation) */
		stage2_output[i] = primary - (noise_ref / 4);
	}

	/* ===== STAGE 3: Wind Noise Reduction ===== */
	/* Detect wind and apply high-pass filter if needed */
	int32_t low_freq_energy = 0;
	for (int i = 0; i < FRAME_SIZE; i += 8) {
		low_freq_energy += (stage2_output[i] * stage2_output[i]) / 256;
	}
	bool wind_detected = (low_freq_energy / (FRAME_SIZE / 8)) > 400;

	for (int i = 0; i < FRAME_SIZE; i++) {
		if (wind_detected && i >= 1) {
			/* High-pass filter */
			stage3_output[i] = stage2_output[i] - stage2_output[i-1];
		} else {
			stage3_output[i] = stage2_output[i];
		}
	}

	/* ===== STAGE 4: Clothing Rustle Suppression ===== */
	for (int i = 2; i < FRAME_SIZE; i++) {
		/* Detect impulse */
		int32_t accel = stage3_output[i] - 2*stage3_output[i-1] + stage3_output[i-2];
		if (accel < 0) accel = -accel;

		if (accel > 500) {
			/* Attenuate impulse */
			stage4_output[i] = stage3_output[i] / 4;
		} else {
			stage4_output[i] = stage3_output[i];
		}
	}

	/* ===== STAGE 5: Enhanced Beamforming with Proximity Detection ===== */
	/* Calculate proximity score */
	int32_t mic_energy[NUM_MICS];
	for (int mic = 0; mic < NUM_MICS; mic++) {
		int32_t energy = 0;
		for (int i = 0; i < FRAME_SIZE; i++) {
			energy += (stage1_output[mic][i] * stage1_output[mic][i]) / 256;
		}
		mic_energy[mic] = energy / FRAME_SIZE;
	}

	int32_t energy_avg = (mic_energy[0] + mic_energy[1] + mic_energy[2]) / 3;
	int32_t energy_diff = 0;
	for (int mic = 0; mic < NUM_MICS; mic++) {
		int32_t diff = mic_energy[mic] - energy_avg;
		if (diff < 0) diff = -diff;
		energy_diff += diff;
	}
	bool near_field = ((energy_diff * 100) / (energy_avg + 1)) > 30;

	/* Apply beamforming (already done in stage4_output) */
	for (int i = 0; i < FRAME_SIZE; i++) {
		stage5_output[i] = stage4_output[i];
	}

	/* ===== STAGE 6: AGC + Chest Resonance-Aware VAD ===== */
	/* Calculate energy and apply AGC */
	int32_t signal_energy = 0;
	for (int i = 0; i < FRAME_SIZE; i++) {
		signal_energy += (stage5_output[i] * stage5_output[i]) / 256;
	}
	int32_t rms = signal_energy / FRAME_SIZE;
	int16_t gain = 128;  /* Unity gain in fixed-point */
	if (rms > 0) {
		gain = (2000 * 256) / (rms + 1);
		if (gain > 512) gain = 512;
		if (gain < 64) gain = 64;
	}

	for (int i = 0; i < FRAME_SIZE; i++) {
		final_output[i] = (stage5_output[i] * gain) / 256;
	}

	/* Chest resonance detection for robust VAD */
	int32_t chest_resonance = 0;
	for (int i = 4; i < FRAME_SIZE; i += 4) {
		int32_t low_freq = (stage1_output[0][i-3] + stage1_output[0][i-2] +
		                    stage1_output[0][i-1] + stage1_output[0][i]) / 4;
		chest_resonance += (low_freq * low_freq) / 256;
	}
	chest_resonance /= (FRAME_SIZE / 4);

	bool voice_detected = near_field && (rms > 500) && (chest_resonance > 300);

	end_us = get_timestamp_us();

	work_result = voice_detected ? final_output[0] : 0;
	return (end_us - start_us) * RISCV_FREQ_MHZ;
}

/* Mixed workload */
static uint64_t workload_mixed(void)
{
	uint64_t cycles = 0;
	cycles += workload_matrix_mult();
	cycles += workload_sorting();
	cycles += workload_fft_sim();
	cycles += workload_crypto_sim();
	return cycles;
}

/* Execute current workload */
static uint64_t execute_workload(void)
{
	switch (current_workload) {
	case WORKLOAD_MATRIX_MULT:
		return workload_matrix_mult();
	case WORKLOAD_SORTING:
		return workload_sorting();
	case WORKLOAD_FFT_SIM:
		return workload_fft_sim();
	case WORKLOAD_CRYPTO_SIM:
		return workload_crypto_sim();
	case WORKLOAD_MIXED:
		return workload_mixed();
	case WORKLOAD_AUDIO_PIPELINE:
		return workload_audio_pipeline();
	case WORKLOAD_AUDIO_PIPELINE_AEC:
		return workload_audio_pipeline_aec();
	case WORKLOAD_PROXIMITY_VAD:
		return workload_proximity_vad();
	case WORKLOAD_CHEST_RESONANCE:
		return workload_chest_resonance();
	case WORKLOAD_CLOTHING_RUSTLE:
		return workload_clothing_rustle();
	case WORKLOAD_SPATIAL_NOISE_CANCEL:
		return workload_spatial_noise_cancel();
	case WORKLOAD_WIND_NOISE_REDUCTION:
		return workload_wind_noise_reduction();
	case WORKLOAD_NECKLACE_FULL:
		return workload_necklace_full();
	case WORKLOAD_IDLE:
	default:
		k_sleep(K_MSEC(100));
		return 0;
	}
}

/* IPC endpoint callback */
static void ep_bound(void *priv)
{
	printk("RISC-V: IPC endpoint bound\n");
}

static void ep_recv(const void *data, size_t len, void *priv)
{
	struct ipc_message *msg = (struct ipc_message *)data;

	printk("RISC-V: Received IPC msg type=%d len=%d\n", msg->type, len);

	if (msg->type == IPC_MSG_SET_WORKLOAD) {
		current_workload = msg->workload;
		printk("RISC-V: Workload changed to %d\n", current_workload);

		/* Reset stats */
		total_work_cycles = 0;
		work_iterations = 0;
	} else {
		printk("RISC-V: Unknown message type %d\n", msg->type);
	}
}

static struct ipc_ept_cfg ep_cfg = {
	.name = "ep0",
	.cb = {
		.bound    = ep_bound,
		.received = ep_recv,
	},
};

/* Stats reporting thread */
void stats_thread(void)
{
	struct ipc_message msg;
	struct stats_data *stats;
	uint64_t prev_cycles = 0;
	uint32_t prev_iterations = 0;

	while (1) {
		k_sleep(K_MSEC(STATS_INTERVAL_MS));

		/* Calculate delta stats */
		uint64_t cycle_delta = total_work_cycles - prev_cycles;
		uint32_t iter_delta = work_iterations - prev_iterations;

		prev_cycles = total_work_cycles;
		prev_iterations = work_iterations;

		/* Calculate MIPS */
		/* RISC-V frequency: 128 MHz */
		/* MIPS = (cycles / interval_sec) / 1,000,000 */
		/* Assuming 1.5 cycles per instruction on average */
		uint64_t instructions = (cycle_delta * 10) / 15;
		uint32_t mips = instructions / 1000000;

		/* Calculate CPU utilization percentage */
		/* CPU% = (MIPS / MHz) * 100 */
		uint32_t cpu_pct = (mips * 100) / RISCV_FREQ_MHZ;
		if (cpu_pct > 100) {
			cpu_pct = 100;  /* Cap at 100% */
		}

		/* Send stats via IPC */
		memset(&msg, 0, sizeof(msg));
		msg.type = IPC_MSG_STATS;
		msg.workload = current_workload;

		stats = (struct stats_data *)msg.data;
		stats->total_cycles = cycle_delta;
		stats->iterations = iter_delta;
		stats->mips = mips;
		stats->workload_type = current_workload;
		stats->cpu_pct = cpu_pct;

		int ret = ipc_service_send(&ep, &msg, sizeof(msg));
		if (ret < 0) {
			printk("RISC-V: Failed to send stats (err %d)\n", ret);
		}

		/* Also print locally */
		printk("\n=== RISC-V Stats (Workload: %d) ===\n", current_workload);
		printk("CPU freq: %u MHz\n", RISCV_FREQ_MHZ);
		printk("Est. MIPS: %u\n", mips);
		printk("CPU utilization: %u%%\n", cpu_pct);
		printk("Cycles: %llu\n", cycle_delta);
		printk("Iterations: %u\n", iter_delta);
		printk("=====================================\n\n");
	}
}

/* Workload execution thread */
void workload_thread(void)
{
	uint64_t test_start, test_end;

	printk("RISC-V: Workload thread started\n");

	/* Test timestamp counter */
	test_start = get_timestamp_us();
	k_busy_wait(1000);  /* 1ms busy wait */
	test_end = get_timestamp_us();
	printk("RISC-V: Timestamp test: start=%llu end=%llu delta=%llu us\n",
	       test_start, test_end, test_end - test_start);

	while (1) {
		if (current_workload != WORKLOAD_IDLE) {
			uint64_t cycles = execute_workload();
			total_work_cycles += cycles;
			work_iterations++;

			/* Debug output for first few iterations */
			if (work_iterations <= 3) {
				printk("RISC-V: Iteration %u: cycles=%llu\n", work_iterations, cycles);
			}
		} else {
			k_sleep(K_MSEC(100));
		}
	}
}

K_THREAD_DEFINE(stats_tid, 2048, stats_thread, NULL, NULL, NULL, 7, 0, 0);
K_THREAD_DEFINE(workload_tid, 4096, workload_thread, NULL, NULL, NULL, 7, 0, 0);

int main(void)
{
	int ret;
	const struct device *ipc_instance;

	printk("Starting RISC-V Coprocessor\n");
	printk("CPU Frequency: 128 MHz\n");

	/* Try to get IPC instance - may not exist in some configurations */
	#if DT_NODE_EXISTS(DT_NODELABEL(ipc0))
	ipc_instance = DEVICE_DT_GET(DT_NODELABEL(ipc0));
	if (!device_is_ready(ipc_instance)) {
		printk("WARNING: IPC instance not ready\n");
	} else {
		/* Open IPC endpoint */
		ret = ipc_service_open_instance(ipc_instance);
		if (ret < 0) {
			printk("WARNING: Failed to open IPC instance (err %d)\n", ret);
		} else {
			/* Register endpoint */
			ret = ipc_service_register_endpoint(ipc_instance, &ep, &ep_cfg);
			if (ret < 0) {
				printk("WARNING: Failed to register endpoint (err %d)\n", ret);
			} else {
				printk("RISC-V: IPC initialized\n");
			}
		}
	}
	#else
	printk("WARNING: IPC not configured in device tree\n");
	#endif

	printk("RISC-V: Ready for workload commands\n");

	return 0;
}
