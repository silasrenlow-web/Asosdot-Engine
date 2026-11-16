extends RefCounted

## The trailer's music, synthesised note by note from band-limited wavetables
## and rendered into an AudioStreamWAV before the first frame. It's written to
## the trailer's cut list: one bar is 2.5 seconds at 96 BPM, and every shot
## starts on a bar line, so the hits land on the cuts.

const RATE := 48000
const BPM := 96.0
const BEAT := 60.0 / BPM
const BAR := BEAT * 4.0

var left := PackedFloat32Array()
var right := PackedFloat32Array()
var length := 0.0

var _warm := PackedFloat32Array() # pads and bass: a saw with its top rolled off
var _bright := PackedFloat32Array() # plucks and stabs
var _rng := RandomNumberGenerator.new()

const TABLE_SIZE := 4096
const TABLE_MASK := TABLE_SIZE - 1


func _init(p_length: float) -> void:
	length = p_length
	left.resize(int(length * RATE))
	right.resize(int(length * RATE))
	_rng.seed = 96
	_warm = _make_table(14, 1.35)
	_bright = _make_table(28, 1.0)


## One period of a saw built from its harmonics, so it doesn't alias.
func _make_table(p_harmonics: int, p_rolloff: float) -> PackedFloat32Array:
	var table := PackedFloat32Array()
	table.resize(TABLE_SIZE)
	var peak := 0.0
	for i in TABLE_SIZE:
		var x := float(i) / TABLE_SIZE
		var s := 0.0
		for k in range(1, p_harmonics + 1):
			s += sin(TAU * k * x) / pow(k, p_rolloff)
		table[i] = s
		peak = maxf(peak, absf(s))
	for i in TABLE_SIZE:
		table[i] /= peak
	return table


static func mtof(p_midi: float) -> float:
	return 440.0 * pow(2.0, (p_midi - 69.0) / 12.0)


## A wavetable voice with an attack, a release and an optional exponential
## decay (seconds to fall by 60 dB; zero holds).
func _voice(p_table: PackedFloat32Array, p_freq: float, p_start: float, p_dur: float, p_amp: float, p_pan: float, p_attack: float, p_release: float, p_decay := 0.0) -> void:
	var first := int(p_start * RATE)
	var count := mini(int((p_dur + p_release) * RATE), left.size() - first)
	if count <= 0:
		return
	var gain_l := p_amp * cos((p_pan + 1.0) * PI * 0.25)
	var gain_r := p_amp * sin((p_pan + 1.0) * PI * 0.25)
	var phase := _rng.randf()
	var step := p_freq / RATE
	var attack := maxf(p_attack, 0.002) * RATE
	var hold := p_dur * RATE
	var release := maxf(p_release, 0.002) * RATE
	var decay := 1.0
	var decay_mul := 1.0 if p_decay <= 0.0 else pow(0.001, 1.0 / (p_decay * RATE))
	for k in count:
		var env := minf(float(k) / attack, 1.0) * decay
		if k > hold:
			env *= maxf(1.0 - (k - hold) / release, 0.0)
		decay *= decay_mul
		var p := phase * TABLE_SIZE
		var i := int(p)
		var f := p - i
		var s := lerpf(p_table[i & TABLE_MASK], p_table[(i + 1) & TABLE_MASK], f) * env
		left[first + k] += s * gain_l
		right[first + k] += s * gain_r
		phase += step
		if phase >= 1.0:
			phase -= 1.0


func pad(p_notes: Array, p_start: float, p_dur: float, p_amp := 0.05) -> void:
	for n in p_notes.size():
		var freq := mtof(p_notes[n])
		var pan := lerpf(-0.6, 0.6, float(n) / maxf(p_notes.size() - 1, 1))
		# Two voices a few cents apart give the slow beating of a string pad.
		_voice(_warm, freq * 1.0035, p_start, p_dur, p_amp, pan, 1.2, 1.6)
		_voice(_warm, freq * 0.9965, p_start, p_dur, p_amp, -pan * 0.5, 1.2, 1.6)


func bass(p_midi: int, p_start: float, p_dur: float, p_amp := 0.16) -> void:
	_voice(_warm, mtof(p_midi), p_start, p_dur, p_amp, 0.0, 0.02, 0.3)
	_sine(mtof(p_midi - 12), p_start, p_dur, p_amp * 0.8, 0.02, 0.3)


func pluck(p_midi: int, p_start: float, p_amp := 0.07, p_pan := 0.0) -> void:
	_voice(_bright, mtof(p_midi), p_start, 0.05, p_amp, p_pan, 0.003, 0.9, 0.9)


func stab(p_notes: Array, p_start: float, p_amp := 0.08) -> void:
	for n in p_notes:
		_voice(_bright, mtof(n) * 1.004, p_start, 0.12, p_amp, -0.3, 0.004, 0.5, 0.7)
		_voice(_bright, mtof(n) * 0.996, p_start, 0.12, p_amp, 0.3, 0.004, 0.5, 0.7)


func _sine(p_freq: float, p_start: float, p_dur: float, p_amp: float, p_attack: float, p_release: float) -> void:
	var first := int(p_start * RATE)
	var count := mini(int((p_dur + p_release) * RATE), left.size() - first)
	var hold := p_dur * RATE
	var w := TAU * p_freq / RATE
	for k in count:
		var env := minf(float(k) / (p_attack * RATE), 1.0)
		if k > hold:
			env *= maxf(1.0 - (k - hold) / (p_release * RATE), 0.0)
		var s := sin(w * k) * env * p_amp
		left[first + k] += s
		right[first + k] += s


## A sine whose pitch falls from p_from to p_to: kicks, toms and booms.
func _drop(p_start: float, p_from: float, p_to: float, p_sweep: float, p_decay: float, p_amp: float) -> void:
	var first := int(p_start * RATE)
	var count := mini(int(p_decay * 1.5 * RATE), left.size() - first)
	var phase := 0.0
	for k in count:
		var t := float(k) / RATE
		var freq := p_to + (p_from - p_to) * exp(-t / p_sweep)
		phase += TAU * freq / RATE
		var s := sin(phase) * exp(-t * 6.9 / p_decay) * minf(t * 400.0, 1.0) * p_amp
		left[first + k] += s
		right[first + k] += s


func _noise(p_start: float, p_dur: float, p_amp: float, p_cutoff_from: float, p_cutoff_to: float, p_attack: float, p_decay: float, p_highpass := false) -> void:
	var first := int(p_start * RATE)
	var count := mini(int(p_dur * RATE), left.size() - first)
	var lp_l := 0.0
	var lp_r := 0.0
	for k in count:
		var t := float(k) / RATE
		var cutoff := lerpf(p_cutoff_from, p_cutoff_to, t / p_dur)
		var a := 1.0 - exp(-TAU * cutoff / RATE)
		var env := minf(t / maxf(p_attack, 0.0005), 1.0) * (exp(-t * 6.9 / p_decay) if p_decay > 0.0 else 1.0)
		var nl := _rng.randf_range(-1.0, 1.0)
		var nr := _rng.randf_range(-1.0, 1.0)
		lp_l += (nl - lp_l) * a
		lp_r += (nr - lp_r) * a
		var sl := (nl - lp_l) if p_highpass else lp_l
		var sr := (nr - lp_r) if p_highpass else lp_r
		left[first + k] += sl * env * p_amp
		right[first + k] += sr * env * p_amp


func kick(p_start: float, p_amp := 0.55) -> void:
	_drop(p_start, 150.0, 46.0, 0.03, 0.45, p_amp)
	_noise(p_start, 0.02, p_amp * 0.25, 3000.0, 3000.0, 0.0005, 0.01)


func tom(p_start: float, p_amp := 0.35) -> void:
	_drop(p_start, 220.0, 80.0, 0.06, 0.6, p_amp)


func hat(p_start: float, p_amp := 0.05) -> void:
	_noise(p_start, 0.06, p_amp, 7000.0, 7000.0, 0.0005, 0.04, true)


## The cinematic impact: a sub drop, a dark noise burst and a long tail.
func boom(p_start: float, p_amp := 0.7) -> void:
	_drop(p_start, 90.0, 30.0, 0.25, 3.5, p_amp)
	_noise(p_start, 2.5, p_amp * 0.6, 900.0, 120.0, 0.002, 1.2)


## Builds into a cut: noise opening up and a rising tone.
func riser(p_start: float, p_dur: float, p_amp := 0.22) -> void:
	_noise(p_start, p_dur, p_amp, 150.0, 9000.0, p_dur * 0.9, 0.0)
	var first := int(p_start * RATE)
	var count := mini(int(p_dur * RATE), left.size() - first)
	var phase := 0.0
	for k in count:
		var t := float(k) / count
		phase += TAU * lerpf(110.0, 880.0, t * t) / RATE
		var s := sin(phase) * t * t * p_amp * 0.5
		left[first + k] += s
		right[first + k] += s


func whoosh(p_at: float, p_amp := 0.12) -> void:
	_noise(p_at - 0.45, 0.5, p_amp, 400.0, 5000.0, 0.42, 0.0)


func bell(p_midi: int, p_start: float, p_amp := 0.08, p_pan := 0.0) -> void:
	var freq := mtof(p_midi)
	var first := int(p_start * RATE)
	var count := mini(int(4.0 * RATE), left.size() - first)
	var gain_l := cos((p_pan + 1.0) * PI * 0.25)
	var gain_r := sin((p_pan + 1.0) * PI * 0.25)
	for k in count:
		var t := float(k) / RATE
		var s := (sin(TAU * freq * t) * exp(-t * 1.6) + 0.5 * sin(TAU * freq * 2.76 * t) * exp(-t * 3.5) + 0.25 * sin(TAU * freq * 5.4 * t) * exp(-t * 7.0)) * minf(t * 300.0, 1.0) * p_amp
		left[first + k] += s * gain_l
		right[first + k] += s * gain_r


## The cue, bar by bar. The bar numbers match the shot list in trailer.gd.
func compose() -> void:
	# A minor: Am, F, C, G, voiced close so the pad moves little between them.
	var chords := [[57, 60, 64], [53, 57, 60], [55, 60, 64], [55, 59, 62]]
	var roots := [45, 41, 48, 43]
	var arp := [0, 1, 2, 3, 2, 1, 2, 3]

	# Bars 0-3, the title: a low drone that swells, and the impact as the
	# name appears.
	_voice(_warm, mtof(33), 0.0, 7.0, 0.09, 0.0, 2.5, 1.0)
	_voice(_warm, mtof(40), 0.5, 6.5, 0.05, 0.2, 2.5, 1.0)
	pad([57, 64, 69], 0.5, 6.5, 0.022)
	boom(1 * BAR, 0.8)
	bell(81, 1 * BAR + 0.02, 0.05, 0.3)
	whoosh(3 * BAR)

	for bar in range(3, 21):
		var t := bar * BAR
		var c: int = (bar - 3) % 4
		var chord: Array = chords[c]
		var tones: Array = chord + [chord[0] + 12]
		var build := clampf(float(bar - 3) / 16.0, 0.0, 1.0)
		pad(chord, t, BAR, 0.026 + 0.012 * build)
		# Bars 3-8, the gallery: the pad and a pluck arpeggio.
		for i in 8:
			pluck(tones[arp[i]] + 12, t + i * BEAT * 0.5, 0.04 + 0.03 * build, -0.4 if i % 2 else 0.4)
		if bar >= 8:
			# Bars 8-12, the courtyard: bass and a half-time kick.
			bass(roots[c], t, BAR * 0.92)
		if bar >= 8 and bar < 19:
			kick(t)
			kick(t + 2 * BEAT, 0.45)
		if bar >= 12 and bar < 19:
			# Bars 12-15, the lanterns: hats.
			for i in 16:
				hat(t + i * BEAT * 0.25, 0.035 if i % 2 else 0.05)
		if bar >= 15 and bar < 19:
			# Bars 15-19, the terrain: every beat, toms into each bar line.
			kick(t + BEAT, 0.4)
			kick(t + 3 * BEAT, 0.4)
			tom(t + 3.5 * BEAT, 0.25)
			tom(t + 3.75 * BEAT, 0.3)

	for cut in [8, 12, 15]:
		whoosh(cut * BAR, 0.1)

	# Bars 19-21, the flare: drums drop out and everything rises.
	riser(19 * BAR, 2 * BAR - 0.1)

	# Bars 21-23, the montage: a hit on every cut.
	boom(21 * BAR, 0.75)
	for i in 8:
		var chord: Array = chords[0] if i < 4 else chords[(i / 2) % 4]
		stab(chord, 21 * BAR + i * BEAT, 0.05)
		kick(21 * BAR + i * BEAT, 0.5)
		bass(roots[0] if i < 4 else roots[(i / 2) % 4], 21 * BAR + i * BEAT, BEAT * 0.8, 0.12)

	# Bars 23-27, the end card: the last impact, a held chord and a bell line.
	boom(23 * BAR, 0.9)
	pad([45, 52, 57, 59, 60, 64], 23 * BAR, 7.0, 0.03)
	_voice(_warm, mtof(33), 23 * BAR, 7.0, 0.06, 0.0, 0.05, 1.8)
	var melody := [[76, 0.5], [74, 1.1], [72, 1.7], [71, 2.3], [69, 3.4]]
	for m in melody:
		bell(m[0], 23 * BAR + m[1], 0.06, 0.25)


## Scales the mix to leave headroom and packs it as 16-bit stereo.
func to_stream() -> AudioStreamWAV:
	var peak := 0.0
	for i in left.size():
		peak = maxf(peak, maxf(absf(left[i]), absf(right[i])))
	var scale := 0.89 / maxf(peak, 0.0001)
	var data := PackedByteArray()
	data.resize(left.size() * 4)
	for i in left.size():
		data.encode_s16(i * 4, int(clampf(left[i] * scale, -1.0, 1.0) * 32767.0))
		data.encode_s16(i * 4 + 2, int(clampf(right[i] * scale, -1.0, 1.0) * 32767.0))
	var stream := AudioStreamWAV.new()
	stream.format = AudioStreamWAV.FORMAT_16_BITS
	stream.mix_rate = RATE
	stream.stereo = true
	stream.data = data
	return stream
