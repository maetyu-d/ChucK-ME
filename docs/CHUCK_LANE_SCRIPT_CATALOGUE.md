# ChucK-ME Lane Script Catalogue

This is a catalogue of lane scripts for ChucK-ME. Each recipe is designed for the lane code editor and includes the two required sections:

```chuck
// wf::declaration
...

// wf::control
...
```

## How To Use

Copy a whole recipe into a lane code editor, replacing the existing code, then press Run.

Most recipes use the placeholder suffix `N`. Before pressing Run, replace every `N` with the lane's zero-based index:

- Lane 1 uses `0`
- Lane 2 uses `1`
- Lane 3 uses `2`
- Lane 8 uses `7`

For example, `ckN` becomes `ck0` in lane 1, and `laneActiveN` becomes `laneActive0`.

If you use the same recipe twice in one track, rename the short prefix as well, for example `ck0` to `ckA0`, so the ChucK variables remain unique.

## Host Variables

The control block can use these values supplied by ChucK-ME:

- `tick`: the lane-local tick count
- `didTick`: `1` on a new lane tick, otherwise `0`
- `stepPhase`: phase from `0.0` to `1.0` between ticks
- `laneActiveN`: `1.0` while this lane is active, `0.0` outside its duration or before phase start
- `intensity`: global performance intensity, `0.0` to `1.0`
- `bright`: global brightness, `0.0` to `1.0`
- `orbit`: global orbit phase, `0.0` to `1.0`

The examples route audio to `master`, so mixer gain and master volume still work.

## Drums And Percussion

### 1. Deep 808 Kick

```chuck
// wf::declaration
SinOsc ckNBody => Gain ckNBodyGain => Pan2 ckNPan => master;
TriOsc ckNClick => Gain ckNClickGain => ckNPan;
0.0 => ckNBodyGain.gain;
0.0 => ckNClickGain.gain;
0.0 => float ckNEnv;
0.0 => float ckNClickEnv;

// wf::control
if (didTick == 1)
{
    tick % 16 => int ckNStep;
    if (ckNStep == 0 || ckNStep == 8 || ckNStep == 14)
    {
        1.0 => ckNEnv;
        1.0 => ckNClickEnv;
    }
}

ckNEnv * 0.955 => ckNEnv;
ckNClickEnv * 0.32 => ckNClickEnv;
40.0 + (ckNEnv * 78.0) => ckNBody.freq;
1400.0 + (bright * 900.0) => ckNClick.freq;
laneActiveN * (0.78 + intensity * 0.44) * ckNEnv => ckNBodyGain.gain;
laneActiveN * (0.06 + bright * 0.08) * ckNClickEnv => ckNClickGain.gain;
0.0 => ckNPan.pan;
```

### 2. Soft Sub Kick

```chuck
// wf::declaration
SinOsc skN => Gain skNGain => Pan2 skNPan => master;
0.0 => skNGain.gain;
0.0 => float skNEnv;

// wf::control
if (didTick == 1)
{
    tick % 16 => int skNStep;
    if (skNStep == 0 || skNStep == 10)
        1.0 => skNEnv;
}

skNEnv * 0.970 => skNEnv;
34.0 + (skNEnv * 42.0) => skN.freq;
laneActiveN * 0.72 * skNEnv => skNGain.gain;
0.0 => skNPan.pan;
```

### 3. Distorted Snare

```chuck
// wf::declaration
Noise dsNNoise => BPF dsNBpf => Gain dsNNoiseGain => Pan2 dsNPan => master;
SqrOsc dsNTone => Gain dsNToneGain => dsNPan;
0.0 => dsNNoiseGain.gain;
0.0 => dsNToneGain.gain;
5.0 => dsNBpf.Q;
0.0 => float dsNEnv;
0.0 => float dsNSnap;

// wf::control
if (didTick == 1)
{
    tick % 16 => int dsNStep;
    if (dsNStep == 4 || dsNStep == 12)
    {
        1.0 => dsNEnv;
        1.0 => dsNSnap;
    }
}

dsNEnv * 0.76 => dsNEnv;
dsNSnap * 0.42 => dsNSnap;
185.0 + (dsNEnv * 90.0) => dsNTone.freq;
1600.0 + (bright * 3200.0) => dsNBpf.freq;
laneActiveN * (0.30 + intensity * 0.25) * dsNEnv => dsNToneGain.gain;
laneActiveN * (0.55 + bright * 0.28) * dsNSnap => dsNNoiseGain.gain;
0.0 => dsNPan.pan;
```

### 4. Dry Clap

```chuck
// wf::declaration
Noise clNNoise => HPF clNHpf => Gain clNGain => Pan2 clNPan => master;
0.0 => clNGain.gain;
900.0 => clNHpf.freq;
0.0 => float clNEnv;
0.0 => float clNDelay;

// wf::control
if (didTick == 1)
{
    tick % 16 => int clNStep;
    if (clNStep == 4 || clNStep == 12)
    {
        1.0 => clNEnv;
        0.0 => clNDelay;
    }
}

clNDelay + 1.0 => clNDelay;
clNEnv * 0.70 => clNEnv;
if (clNDelay == 2.0 || clNDelay == 4.0)
    Math.max(clNEnv, 0.66) => clNEnv;

1200.0 + (bright * 2600.0) => clNHpf.freq;
laneActiveN * (0.20 + bright * 0.22) * clNEnv => clNGain.gain;
0.08 => clNPan.pan;
```

### 5. Closed Hat Ticker

```chuck
// wf::declaration
Noise hhN => HPF hhNHpf => Gain hhNGain => Pan2 hhNPan => master;
0.0 => hhNGain.gain;
0.0 => float hhNEnv;

// wf::control
if (didTick == 1)
{
    tick % 4 => int hhNStep;
    if (hhNStep == 0 || hhNStep == 2 || (hhNStep == 3 && intensity > 0.55))
        1.0 => hhNEnv;
}

hhNEnv * (0.40 + bright * 0.08) => hhNEnv;
5200.0 + (bright * 4800.0) => hhNHpf.freq;
laneActiveN * (0.035 + intensity * 0.045) * hhNEnv => hhNGain.gain;
0.18 => hhNPan.pan;
```

### 6. Open Hat Wash

```chuck
// wf::declaration
Noise ohN => HPF ohNHpf => Gain ohNGain => Pan2 ohNPan => master;
0.0 => ohNGain.gain;
0.0 => float ohNEnv;

// wf::control
if (didTick == 1)
{
    tick % 16 => int ohNStep;
    if (ohNStep == 6 || ohNStep == 14)
        1.0 => ohNEnv;
}

ohNEnv * 0.88 => ohNEnv;
3600.0 + (bright * 3800.0) => ohNHpf.freq;
laneActiveN * (0.05 + bright * 0.07) * ohNEnv => ohNGain.gain;
-0.20 => ohNPan.pan;
```

### 7. Metallic FM Percussion

```chuck
// wf::declaration
SinOsc mpNMod => SinOsc mpNCar => Gain mpNGain => Pan2 mpNPan => master;
0.0 => mpNGain.gain;
0.0 => float mpNEnv;

// wf::control
if (didTick == 1)
{
    tick % 8 => int mpNStep;
    if (mpNStep == 1 || mpNStep == 5 || (mpNStep == 7 && intensity > 0.65))
        1.0 => mpNEnv;
}

mpNEnv * 0.72 => mpNEnv;
380.0 + (tick % 5) * 91.0 => mpNMod.freq;
700.0 + (mpNEnv * 900.0) + (bright * 1200.0) + (mpNMod.last() * (120.0 + bright * 260.0)) => mpNCar.freq;
laneActiveN * (0.10 + intensity * 0.14) * mpNEnv => mpNGain.gain;
0.32 => mpNPan.pan;
```

### 8. Glitch Tick Dust

```chuck
// wf::declaration
TriOsc gdNOsc => Gain gdNGain => Pan2 gdNPan => master;
0.0 => gdNGain.gain;
0.0 => float gdNEnv;

// wf::control
if (didTick == 1)
{
    tick % 16 => int gdNStep;
    if (gdNStep == 1 || gdNStep == 2 || gdNStep == 7 || gdNStep == 11 || gdNStep == 15)
        1.0 => gdNEnv;
}

gdNEnv * 0.36 => gdNEnv;
900.0 + ((tick * 317) % 1600) + (bright * 1200.0) => gdNOsc.freq;
laneActiveN * (0.035 + intensity * 0.050) * gdNEnv => gdNGain.gain;
Math.sin(tick * 0.71) * 0.42 => gdNPan.pan;
```

## Bass

### 9. Warm Saw Bass

```chuck
// wf::declaration
SawOsc wbNOsc => LPF wbNLpf => Gain wbNGain => Pan2 wbNPan => master;
0.75 => wbNLpf.Q;
0.0 => wbNGain.gain;
0.0 => float wbNEnv;
55.0 => float wbNFreq;

// wf::control
if (didTick == 1)
{
    tick % 8 => int wbNStep;
    if (wbNStep == 0) 55.0 => wbNFreq;
    else if (wbNStep == 2) 82.41 => wbNFreq;
    else if (wbNStep == 4) 73.42 => wbNFreq;
    else if (wbNStep == 6) 65.41 => wbNFreq;
    if (wbNStep == 0 || wbNStep == 2 || wbNStep == 4 || wbNStep == 6)
        1.0 => wbNEnv;
}

wbNEnv * 0.91 => wbNEnv;
wbNFreq * (0.995 + orbit * 0.010) => wbNOsc.freq;
280.0 + (bright * 1500.0) + (wbNEnv * 420.0) => wbNLpf.freq;
laneActiveN * (0.20 + intensity * 0.18) * wbNEnv => wbNGain.gain;
0.0 => wbNPan.pan;
```

### 10. Acid Pulse Bass

```chuck
// wf::declaration
SqrOsc abNOsc => LPF abNLpf => Gain abNGain => Pan2 abNPan => master;
1.0 => abNLpf.Q;
0.0 => abNGain.gain;
0.0 => float abNEnv;
82.41 => float abNFreq;

// wf::control
if (didTick == 1)
{
    tick % 16 => int abNStep;
    if (abNStep == 0) 82.41 => abNFreq;
    else if (abNStep == 3) 98.00 => abNFreq;
    else if (abNStep == 6) 73.42 => abNFreq;
    else if (abNStep == 10) 110.00 => abNFreq;
    else if (abNStep == 14) 65.41 => abNFreq;
    if (abNStep == 0 || abNStep == 3 || abNStep == 6 || abNStep == 10 || abNStep == 14)
        1.0 => abNEnv;
}

abNEnv * 0.82 => abNEnv;
abNFreq => abNOsc.freq;
220.0 + (abNEnv * 1900.0) + (bright * 1400.0) => abNLpf.freq;
laneActiveN * (0.16 + intensity * 0.22) * abNEnv => abNGain.gain;
-0.03 => abNPan.pan;
```

### 11. Rubber FM Bass

```chuck
// wf::declaration
SinOsc rbNMod => SinOsc rbNCar => LPF rbNLpf => Gain rbNGain => Pan2 rbNPan => master;
0.0 => rbNGain.gain;
0.0 => float rbNEnv;
55.0 => float rbNFreq;

// wf::control
if (didTick == 1)
{
    tick % 8 => int rbNStep;
    if (rbNStep == 0) 55.0 => rbNFreq;
    else if (rbNStep == 3) 73.42 => rbNFreq;
    else if (rbNStep == 5) 65.41 => rbNFreq;
    else if (rbNStep == 7) 82.41 => rbNFreq;
    if (rbNStep == 0 || rbNStep == 3 || rbNStep == 5 || rbNStep == 7)
        1.0 => rbNEnv;
}

rbNEnv * 0.88 => rbNEnv;
rbNFreq * 2.0 => rbNMod.freq;
rbNFreq + (rbNMod.last() * (18.0 + bright * 42.0) * rbNEnv) => rbNCar.freq;
420.0 + (bright * 1100.0) => rbNLpf.freq;
laneActiveN * (0.20 + intensity * 0.18) * rbNEnv => rbNGain.gain;
0.0 => rbNPan.pan;
```

### 12. Reese Bass

```chuck
// wf::declaration
SawOsc reNA => Gain reNMix => LPF reNLpf => Gain reNGain => Pan2 reNPan => master;
SawOsc reNB => reNMix;
0.46 => reNMix.gain;
0.0 => reNGain.gain;
0.0 => float reNEnv;
43.65 => float reNFreq;

// wf::control
if (didTick == 1)
{
    tick % 16 => int reNStep;
    if (reNStep == 0 || reNStep == 8)
    {
        if (reNStep == 0) 43.65 => reNFreq;
        else 49.00 => reNFreq;
        1.0 => reNEnv;
    }
}

reNEnv * 0.975 => reNEnv;
reNFreq * 0.996 => reNA.freq;
reNFreq * 1.006 => reNB.freq;
180.0 + (bright * 900.0) + (Math.sin(orbit * 6.28318) * 70.0) => reNLpf.freq;
laneActiveN * (0.18 + intensity * 0.17) * reNEnv => reNGain.gain;
0.0 => reNPan.pan;
```

### 13. Pluck Sub Bass

```chuck
// wf::declaration
SinOsc psN => Gain psNGain => Pan2 psNPan => master;
0.0 => psNGain.gain;
0.0 => float psNEnv;

// wf::control
if (didTick == 1)
{
    tick % 12 => int psNStep;
    if (psNStep == 0 || psNStep == 5 || psNStep == 9)
        1.0 => psNEnv;
}

psNEnv * 0.84 => psNEnv;
49.0 * (1.0 + ((tick % 3) * 0.25)) => psN.freq;
laneActiveN * (0.22 + intensity * 0.14) * psNEnv => psNGain.gain;
0.0 => psNPan.pan;
```

## Melodic And Arpeggio

### 14. Glass Arp

```chuck
// wf::declaration
TriOsc gaNOsc => Gain gaNGain => Pan2 gaNPan => master;
0.0 => gaNGain.gain;
0.0 => float gaNEnv;
261.63 => float gaNFreq;

// wf::control
if (didTick == 1)
{
    tick % 8 => int gaNStep;
    if (gaNStep == 0) 261.63 => gaNFreq;
    else if (gaNStep == 1) 329.63 => gaNFreq;
    else if (gaNStep == 2) 392.00 => gaNFreq;
    else if (gaNStep == 3) 523.25 => gaNFreq;
    else if (gaNStep == 4) 392.00 => gaNFreq;
    else if (gaNStep == 5) 329.63 => gaNFreq;
    else if (gaNStep == 6) 293.66 => gaNFreq;
    else 392.00 => gaNFreq;
    1.0 => gaNEnv;
}

gaNEnv * 0.70 => gaNEnv;
gaNFreq * (0.996 + bright * 0.010) => gaNOsc.freq;
laneActiveN * (0.10 + intensity * 0.09) * gaNEnv => gaNGain.gain;
Math.sin(orbit * 6.28318) * 0.18 => gaNPan.pan;
```

### 15. Kraft Pulse Arp

```chuck
// wf::declaration
SqrOsc kaNOsc => Gain kaNGain => Pan2 kaNPan => master;
0.0 => kaNGain.gain;
0.0 => float kaNEnv;
220.0 => float kaNFreq;

// wf::control
if (didTick == 1)
{
    tick % 16 => int kaNStep;
    if (kaNStep == 0) 220.0 => kaNFreq;
    else if (kaNStep == 2) 277.18 => kaNFreq;
    else if (kaNStep == 4) 329.63 => kaNFreq;
    else if (kaNStep == 6) 440.0 => kaNFreq;
    else if (kaNStep == 8) 329.63 => kaNFreq;
    else if (kaNStep == 10) 277.18 => kaNFreq;
    else if (kaNStep == 12) 246.94 => kaNFreq;
    else if (kaNStep == 14) 329.63 => kaNFreq;
    if (kaNStep % 2 == 0)
        1.0 => kaNEnv;
}

kaNEnv * 0.62 => kaNEnv;
kaNFreq => kaNOsc.freq;
laneActiveN * (0.08 + bright * 0.07) * kaNEnv => kaNGain.gain;
0.22 => kaNPan.pan;
```

### 16. Random Walk Blips

```chuck
// wf::declaration
SinOsc rwNOsc => Gain rwNGain => Pan2 rwNPan => master;
0.0 => rwNGain.gain;
0.0 => float rwNEnv;
0 => int rwNNote;

// wf::control
if (didTick == 1)
{
    if (tick % 2 == 0)
    {
        ((rwNNote + ((tick % 5) - 2)) + 8) % 8 => rwNNote;
        1.0 => rwNEnv;
    }
}

rwNEnv * 0.58 => rwNEnv;
220.0 * Math.pow(2.0, (rwNNote * 2.0 + 12.0) / 12.0) => rwNOsc.freq;
laneActiveN * (0.07 + intensity * 0.10) * rwNEnv => rwNGain.gain;
Math.sin(tick * 0.31) * 0.48 => rwNPan.pan;
```

### 17. FM Bell Arp

```chuck
// wf::declaration
SinOsc fbNMod => SinOsc fbNCar => Gain fbNGain => Pan2 fbNPan => master;
0.0 => fbNGain.gain;
0.0 => float fbNEnv;
440.0 => float fbNFreq;

// wf::control
if (didTick == 1)
{
    tick % 8 => int fbNStep;
    if (fbNStep == 0) 440.0 => fbNFreq;
    else if (fbNStep == 2) 554.37 => fbNFreq;
    else if (fbNStep == 4) 659.25 => fbNFreq;
    else if (fbNStep == 6) 880.0 => fbNFreq;
    if (fbNStep == 0 || fbNStep == 2 || fbNStep == 4 || fbNStep == 6)
        1.0 => fbNEnv;
}

fbNEnv * 0.90 => fbNEnv;
fbNFreq * 2.01 => fbNMod.freq;
fbNFreq + (fbNMod.last() * (80.0 + bright * 160.0) * fbNEnv) => fbNCar.freq;
laneActiveN * (0.05 + bright * 0.07) * fbNEnv => fbNGain.gain;
-0.26 => fbNPan.pan;
```

### 18. Chiptune Lead

```chuck
// wf::declaration
SqrOsc chNOsc => Gain chNGain => Pan2 chNPan => master;
0.0 => chNGain.gain;
0.0 => float chNEnv;
329.63 => float chNFreq;

// wf::control
if (didTick == 1)
{
    tick % 16 => int chNStep;
    if (chNStep == 0) 329.63 => chNFreq;
    else if (chNStep == 1) 392.00 => chNFreq;
    else if (chNStep == 2) 493.88 => chNFreq;
    else if (chNStep == 4) 659.25 => chNFreq;
    else if (chNStep == 8) 587.33 => chNFreq;
    else if (chNStep == 12) 493.88 => chNFreq;
    1.0 => chNEnv;
}

chNEnv * 0.68 => chNEnv;
chNFreq * (1.0 + Math.sin(stepPhase * 6.28318) * 0.002) => chNOsc.freq;
laneActiveN * (0.06 + intensity * 0.09) * chNEnv => chNGain.gain;
0.12 => chNPan.pan;
```

### 19. Slow Melody Dots

```chuck
// wf::declaration
TriOsc mdNOsc => Gain mdNGain => Pan2 mdNPan => master;
0.0 => mdNGain.gain;
0.0 => float mdNEnv;
523.25 => float mdNFreq;

// wf::control
if (didTick == 1)
{
    tick % 32 => int mdNStep;
    if (mdNStep == 0) { 523.25 => mdNFreq; 1.0 => mdNEnv; }
    if (mdNStep == 7) { 659.25 => mdNFreq; 1.0 => mdNEnv; }
    if (mdNStep == 13) { 587.33 => mdNFreq; 1.0 => mdNEnv; }
    if (mdNStep == 20) { 783.99 => mdNFreq; 1.0 => mdNEnv; }
}

mdNEnv * 0.94 => mdNEnv;
mdNFreq => mdNOsc.freq;
laneActiveN * (0.08 + bright * 0.05) * mdNEnv => mdNGain.gain;
Math.sin(orbit * 6.28318) * 0.34 => mdNPan.pan;
```

## Chords And Pads

### 20. Soft Major Chord

```chuck
// wf::declaration
SinOsc smNA => Gain smNMix => Gain smNGain => Pan2 smNPan => master;
SinOsc smNB => smNMix;
SinOsc smNC => smNMix;
0.33 => smNMix.gain;
0.0 => smNGain.gain;
0.0 => float smNLevel;

// wf::control
0.16 + (intensity * 0.12) => float smNTarget;
smNLevel + (((laneActiveN * smNTarget) - smNLevel) * 0.018) => smNLevel;
261.63 * (1.0 + orbit * 0.003) => smNA.freq;
329.63 * (1.0 - orbit * 0.002) => smNB.freq;
392.00 * (1.0 + bright * 0.003) => smNC.freq;
smNLevel => smNGain.gain;
-0.10 => smNPan.pan;
```

### 21. Minor Glass Pad

```chuck
// wf::declaration
SinOsc mgNA => Gain mgNMix => Gain mgNGain => Pan2 mgNPan => master;
SinOsc mgNB => mgNMix;
SinOsc mgNC => mgNMix;
0.30 => mgNMix.gain;
0.0 => mgNGain.gain;
0.0 => float mgNLevel;

// wf::control
mgNLevel + (((laneActiveN * (0.12 + bright * 0.08)) - mgNLevel) * 0.012) => mgNLevel;
220.0 * (1.0 + Math.sin(orbit * 6.28318) * 0.004) => mgNA.freq;
261.63 => mgNB.freq;
329.63 * (1.0 + bright * 0.005) => mgNC.freq;
mgNLevel => mgNGain.gain;
0.16 => mgNPan.pan;
```

### 22. PWM-Like Square Chord

```chuck
// wf::declaration
SqrOsc pwNA => Gain pwNMix => LPF pwNLpf => Gain pwNGain => Pan2 pwNPan => master;
SqrOsc pwNB => pwNMix;
SqrOsc pwNC => pwNMix;
0.24 => pwNMix.gain;
0.0 => pwNGain.gain;
0.0 => float pwNLevel;

// wf::control
pwNLevel + (((laneActiveN * (0.10 + intensity * 0.06)) - pwNLevel) * 0.020) => pwNLevel;
130.81 => pwNA.freq;
164.81 * (1.0 + Math.sin(orbit * 6.28318) * 0.006) => pwNB.freq;
196.00 => pwNC.freq;
520.0 + bright * 1700.0 => pwNLpf.freq;
pwNLevel => pwNGain.gain;
-0.18 => pwNPan.pan;
```

### 23. Slow Shimmer Pad

```chuck
// wf::declaration
SinOsc shNA => Gain shNMix => Gain shNGain => Pan2 shNPan => master;
SinOsc shNB => shNMix;
SinOsc shNC => shNMix;
SinOsc shND => shNMix;
0.22 => shNMix.gain;
0.0 => shNGain.gain;
0.0 => float shNLevel;

// wf::control
shNLevel + (((laneActiveN * (0.08 + bright * 0.05)) - shNLevel) * 0.008) => shNLevel;
392.00 * (1.0 + Math.sin(orbit * 6.28318) * 0.002) => shNA.freq;
493.88 * (1.0 - Math.sin(orbit * 6.28318) * 0.002) => shNB.freq;
587.33 => shNC.freq;
783.99 * (1.0 + bright * 0.004) => shND.freq;
shNLevel => shNGain.gain;
Math.sin(orbit * 6.28318) * 0.26 => shNPan.pan;
```

### 24. Organ Fifths

```chuck
// wf::declaration
TriOsc ogNA => Gain ogNMix => Gain ogNGain => Pan2 ogNPan => master;
TriOsc ogNB => ogNMix;
TriOsc ogNC => ogNMix;
0.28 => ogNMix.gain;
0.0 => ogNGain.gain;
0.0 => float ogNLevel;

// wf::control
ogNLevel + (((laneActiveN * (0.14 + intensity * 0.08)) - ogNLevel) * 0.030) => ogNLevel;
196.0 => ogNA.freq;
293.66 => ogNB.freq;
392.0 * (1.0 + bright * 0.002) => ogNC.freq;
ogNLevel => ogNGain.gain;
0.0 => ogNPan.pan;
```

## Textures And Noise

### 25. Tape Hiss Bed

```chuck
// wf::declaration
Noise thN => BPF thNBpf => Gain thNGain => Pan2 thNPan => master;
0.0 => thNGain.gain;
1.5 => thNBpf.Q;
0.0 => float thNLevel;

// wf::control
thNLevel + (((laneActiveN * (0.025 + bright * 0.035)) - thNLevel) * 0.010) => thNLevel;
2800.0 + Math.sin(orbit * 6.28318) * 700.0 + bright * 2200.0 => thNBpf.freq;
thNLevel => thNGain.gain;
Math.sin(orbit * 6.28318) * 0.50 => thNPan.pan;
```

### 26. Wind Sweep

```chuck
// wf::declaration
Noise wsN => LPF wsNLpf => Gain wsNGain => Pan2 wsNPan => master;
0.0 => wsNGain.gain;
0.0 => float wsNLevel;

// wf::control
wsNLevel + (((laneActiveN * (0.05 + intensity * 0.05)) - wsNLevel) * 0.006) => wsNLevel;
400.0 + (bright * 3200.0) + (Math.sin(orbit * 6.28318) * 220.0) => wsNLpf.freq;
wsNLevel => wsNGain.gain;
Math.sin(orbit * 6.28318) * 0.38 => wsNPan.pan;
```

### 27. Dust Particles

```chuck
// wf::declaration
TriOsc dpNOsc => Gain dpNGain => Pan2 dpNPan => master;
0.0 => dpNGain.gain;
0.0 => float dpNEnv;

// wf::control
if (didTick == 1)
{
    if (((tick * 13) % 17) < 5)
        1.0 => dpNEnv;
}

dpNEnv * 0.28 => dpNEnv;
1200.0 + ((tick * 97) % 3000) + bright * 1000.0 => dpNOsc.freq;
laneActiveN * (0.025 + intensity * 0.035) * dpNEnv => dpNGain.gain;
Math.sin(tick * 0.41) * 0.70 => dpNPan.pan;
```

### 28. Dark Drone

```chuck
// wf::declaration
SawOsc ddNA => Gain ddNMix => LPF ddNLpf => Gain ddNGain => Pan2 ddNPan => master;
SawOsc ddNB => ddNMix;
0.33 => ddNMix.gain;
0.0 => ddNGain.gain;
0.0 => float ddNLevel;

// wf::control
ddNLevel + (((laneActiveN * (0.06 + intensity * 0.04)) - ddNLevel) * 0.007) => ddNLevel;
55.0 * (1.0 + Math.sin(orbit * 6.28318) * 0.010) => ddNA.freq;
55.0 * 1.505 => ddNB.freq;
160.0 + (bright * 420.0) => ddNLpf.freq;
ddNLevel => ddNGain.gain;
0.0 => ddNPan.pan;
```

### 29. Radio Morse

```chuck
// wf::declaration
SinOsc rmNOsc => BPF rmNBpf => Gain rmNGain => Pan2 rmNPan => master;
0.0 => rmNGain.gain;
4.0 => rmNBpf.Q;
0.0 => float rmNEnv;

// wf::control
if (didTick == 1)
{
    tick % 16 => int rmNStep;
    if (rmNStep == 0 || rmNStep == 1 || rmNStep == 5 || rmNStep == 9 || rmNStep == 10 || rmNStep == 14)
        1.0 => rmNEnv;
}

rmNEnv * 0.52 => rmNEnv;
720.0 + ((tick % 3) * 110.0) => rmNOsc.freq;
900.0 + bright * 1800.0 => rmNBpf.freq;
laneActiveN * (0.045 + bright * 0.04) * rmNEnv => rmNGain.gain;
Math.sin(tick * 0.19) * 0.58 => rmNPan.pan;
```

### 30. Rising Noise Alarm

```chuck
// wf::declaration
Noise raNNoise => BPF raNBpf => Gain raNNoiseGain => Pan2 raNPan => master;
SinOsc raNTone => Gain raNToneGain => raNPan;
0.0 => raNNoiseGain.gain;
0.0 => raNToneGain.gain;
3.0 => raNBpf.Q;
0.0 => float raNLevel;

// wf::control
raNLevel + (((laneActiveN * (0.08 + intensity * 0.06)) - raNLevel) * 0.012) => raNLevel;
400.0 + (stepPhase * 800.0) + (orbit * 300.0) => raNTone.freq;
1200.0 + (stepPhase * 3200.0) + (bright * 1400.0) => raNBpf.freq;
raNLevel * 0.38 => raNToneGain.gain;
raNLevel * 0.25 => raNNoiseGain.gain;
Math.sin(orbit * 6.28318) * 0.35 => raNPan.pan;
```

## Hybrid Performance Lanes

### 31. Kick And Bass In One Lane

```chuck
// wf::declaration
SinOsc kbNKick => Gain kbNKickGain => Pan2 kbNPan => master;
SawOsc kbNBass => LPF kbNLpf => Gain kbNBassGain => kbNPan;
0.0 => kbNKickGain.gain;
0.0 => kbNBassGain.gain;
0.0 => float kbNKickEnv;
0.0 => float kbNBassEnv;
55.0 => float kbNBassFreq;

// wf::control
if (didTick == 1)
{
    tick % 16 => int kbNStep;
    if (kbNStep == 0 || kbNStep == 8)
        1.0 => kbNKickEnv;
    if (kbNStep == 0 || kbNStep == 3 || kbNStep == 6 || kbNStep == 10)
    {
        if (kbNStep == 0) 55.0 => kbNBassFreq;
        else if (kbNStep == 3) 73.42 => kbNBassFreq;
        else if (kbNStep == 6) 65.41 => kbNBassFreq;
        else 82.41 => kbNBassFreq;
        1.0 => kbNBassEnv;
    }
}

kbNKickEnv * 0.955 => kbNKickEnv;
kbNBassEnv * 0.84 => kbNBassEnv;
42.0 + kbNKickEnv * 60.0 => kbNKick.freq;
kbNBassFreq => kbNBass.freq;
320.0 + bright * 1300.0 => kbNLpf.freq;
laneActiveN * 0.65 * kbNKickEnv => kbNKickGain.gain;
laneActiveN * 0.18 * kbNBassEnv => kbNBassGain.gain;
0.0 => kbNPan.pan;
```

### 32. Motorik Pulse

```chuck
// wf::declaration
SqrOsc moNPulse => Gain moNGain => Pan2 moNPan => master;
0.0 => moNGain.gain;
0.0 => float moNEnv;

// wf::control
if (didTick == 1)
{
    tick % 4 => int moNStep;
    if (moNStep == 0 || moNStep == 2)
        1.0 => moNEnv;
}

moNEnv * 0.78 => moNEnv;
110.0 * (1.0 + ((tick / 4) % 4) * 0.25) => moNPulse.freq;
laneActiveN * (0.10 + intensity * 0.12) * moNEnv => moNGain.gain;
0.0 => moNPan.pan;
```

### 33. Polyrhythm Ping

```chuck
// wf::declaration
SinOsc ppNOsc => Gain ppNGain => Pan2 ppNPan => master;
0.0 => ppNGain.gain;
0.0 => float ppNEnv;

// wf::control
if (didTick == 1)
{
    tick % 5 => int ppNStep;
    if (ppNStep == 0 || ppNStep == 3)
        1.0 => ppNEnv;
}

ppNEnv * 0.74 => ppNEnv;
660.0 + ((tick % 7) * 55.0) + bright * 440.0 => ppNOsc.freq;
laneActiveN * (0.055 + intensity * 0.065) * ppNEnv => ppNGain.gain;
Math.sin(tick * 0.37) * 0.46 => ppNPan.pan;
```

### 34. Probabilistic Feel Without Random

```chuck
// wf::declaration
TriOsc prNOsc => Gain prNGain => Pan2 prNPan => master;
0.0 => prNGain.gain;
0.0 => float prNEnv;

// wf::control
if (didTick == 1)
{
    ((tick * 37) + (tick / 3)) % 16 => int prNMask;
    if (prNMask < (4 + intensity * 7.0))
        1.0 => prNEnv;
}

prNEnv * 0.50 => prNEnv;
440.0 + ((tick * 73) % 900) => prNOsc.freq;
laneActiveN * (0.035 + bright * 0.055) * prNEnv => prNGain.gain;
Math.sin(tick * 0.53) * 0.55 => prNPan.pan;
```

### 35. Orbit-Modulated Arp Drone

```chuck
// wf::declaration
TriOsc odNA => Gain odNMix => Gain odNGain => Pan2 odNPan => master;
SinOsc odNB => odNMix;
0.35 => odNMix.gain;
0.0 => odNGain.gain;
0.0 => float odNLevel;
220.0 => float odNFreq;

// wf::control
if (didTick == 1)
{
    tick % 8 => int odNStep;
    if (odNStep == 0) 220.0 => odNFreq;
    else if (odNStep == 2) 277.18 => odNFreq;
    else if (odNStep == 5) 329.63 => odNFreq;
    else if (odNStep == 7) 392.0 => odNFreq;
}

odNLevel + (((laneActiveN * (0.08 + intensity * 0.07)) - odNLevel) * 0.032) => odNLevel;
odNFreq * (1.0 + Math.sin(orbit * 6.28318) * 0.015) => odNA.freq;
odNFreq * 2.0 => odNB.freq;
odNLevel => odNGain.gain;
Math.sin(orbit * 6.28318) * 0.44 => odNPan.pan;
```

### 36. Stop-Start Gate

```chuck
// wf::declaration
SawOsc sgNOsc => LPF sgNLpf => Gain sgNGain => Pan2 sgNPan => master;
0.0 => sgNGain.gain;
0.0 => float sgNLevel;

// wf::control
tick % 16 => int sgNStep;
if (sgNStep < 3 || (sgNStep >= 8 && sgNStep < 11))
    sgNLevel + (((laneActiveN * (0.12 + intensity * 0.10)) - sgNLevel) * 0.16) => sgNLevel;
else
    sgNLevel * 0.72 => sgNLevel;

110.0 * (1.0 + ((tick / 16) % 3) * 0.25) => sgNOsc.freq;
240.0 + bright * 1800.0 => sgNLpf.freq;
sgNLevel => sgNGain.gain;
0.0 => sgNPan.pan;
```

## Breaks And IDM

### 37. Amen-Style Ghost Break

```chuck
// wf::declaration
SinOsc agNKick => Gain agNKickGain => Pan2 agNPan => master;
Noise agNSnare => BPF agNBpf => Gain agNSnareGain => agNPan;
Noise agNHat => HPF agNHpf => Gain agNHatGain => agNPan;
0.0 => agNKickGain.gain;
0.0 => agNSnareGain.gain;
0.0 => agNHatGain.gain;
3.8 => agNBpf.Q;
0.0 => float agNKickEnv;
0.0 => float agNSnareEnv;
0.0 => float agNHatEnv;

// wf::control
if (didTick == 1)
{
    tick % 16 => int agNStep;
    if (agNStep == 0 || agNStep == 6 || agNStep == 10)
        1.0 => agNKickEnv;
    if (agNStep == 4 || agNStep == 7 || agNStep == 12 || agNStep == 15)
        1.0 => agNSnareEnv;
    if (agNStep == 2 || agNStep == 3 || agNStep == 5 || agNStep == 8 || agNStep == 11 || agNStep == 14)
        1.0 => agNHatEnv;
}

agNKickEnv * 0.94 => agNKickEnv;
agNSnareEnv * 0.62 => agNSnareEnv;
agNHatEnv * 0.38 => agNHatEnv;
44.0 + agNKickEnv * 70.0 => agNKick.freq;
1700.0 + bright * 2400.0 => agNBpf.freq;
5200.0 + bright * 3200.0 => agNHpf.freq;
laneActiveN * 0.50 * agNKickEnv => agNKickGain.gain;
laneActiveN * (0.24 + intensity * 0.18) * agNSnareEnv => agNSnareGain.gain;
laneActiveN * (0.035 + bright * 0.055) * agNHatEnv => agNHatGain.gain;
0.0 => agNPan.pan;
```

### 38. Stutter Snare Roll

```chuck
// wf::declaration
Noise srNNoise => BPF srNBpf => Gain srNGain => Pan2 srNPan => master;
0.0 => srNGain.gain;
5.0 => srNBpf.Q;
0.0 => float srNEnv;

// wf::control
if (didTick == 1)
{
    tick % 32 => int srNStep;
    if (srNStep >= 24 || srNStep == 8 || srNStep == 16)
        1.0 => srNEnv;
}

srNEnv * (0.44 + bright * 0.10) => srNEnv;
1400.0 + (tick % 8) * 180.0 + bright * 2600.0 => srNBpf.freq;
laneActiveN * (0.08 + intensity * 0.12) * srNEnv => srNGain.gain;
Math.sin(tick * 0.29) * 0.30 => srNPan.pan;
```

### 39. Sliced Break Noise

```chuck
// wf::declaration
Noise sbNNoise => BPF sbNBpf => Gain sbNGain => Pan2 sbNPan => master;
0.0 => sbNGain.gain;
2.4 => sbNBpf.Q;
0.0 => float sbNEnv;

// wf::control
if (didTick == 1)
{
    ((tick * 9) + (tick / 4)) % 16 => int sbNStep;
    if (sbNStep == 0 || sbNStep == 1 || sbNStep == 5 || sbNStep == 9 || sbNStep == 12)
        1.0 => sbNEnv;
}

sbNEnv * 0.46 => sbNEnv;
900.0 + ((tick * 173) % 3600) + bright * 1800.0 => sbNBpf.freq;
laneActiveN * (0.08 + intensity * 0.09) * sbNEnv => sbNGain.gain;
Math.sin(tick * 0.67) * 0.52 => sbNPan.pan;
```

### 40. Jungle Ride Spark

```chuck
// wf::declaration
Noise jrNNoise => HPF jrNHpf => Gain jrNGain => Pan2 jrNPan => master;
0.0 => jrNGain.gain;
0.0 => float jrNEnv;

// wf::control
if (didTick == 1)
{
    tick % 6 => int jrNStep;
    if (jrNStep == 0 || jrNStep == 2 || jrNStep == 5)
        1.0 => jrNEnv;
}

jrNEnv * 0.58 => jrNEnv;
6400.0 + bright * 4200.0 => jrNHpf.freq;
laneActiveN * (0.020 + bright * 0.035 + intensity * 0.020) * jrNEnv => jrNGain.gain;
0.34 => jrNPan.pan;
```

### 41. Micro Kick Fills

```chuck
// wf::declaration
SinOsc mkN => Gain mkNGain => Pan2 mkNPan => master;
0.0 => mkNGain.gain;
0.0 => float mkNEnv;

// wf::control
if (didTick == 1)
{
    tick % 32 => int mkNStep;
    if (mkNStep == 0 || mkNStep == 11 || mkNStep == 18 || mkNStep == 26 || mkNStep == 29)
        1.0 => mkNEnv;
}

mkNEnv * 0.90 => mkNEnv;
38.0 + mkNEnv * 95.0 => mkN.freq;
laneActiveN * (0.24 + intensity * 0.22) * mkNEnv => mkNGain.gain;
0.0 => mkNPan.pan;
```

### 42. Drill Ratchet Clicks

```chuck
// wf::declaration
TriOsc rcNOsc => Gain rcNGain => Pan2 rcNPan => master;
0.0 => rcNGain.gain;
0.0 => float rcNEnv;

// wf::control
if (didTick == 1)
{
    tick % 24 => int rcNStep;
    if (rcNStep > 15 || rcNStep == 3 || rcNStep == 9)
        1.0 => rcNEnv;
}

rcNEnv * 0.30 => rcNEnv;
1200.0 + ((tick * 211) % 2600) + bright * 800.0 => rcNOsc.freq;
laneActiveN * (0.030 + intensity * 0.045) * rcNEnv => rcNGain.gain;
Math.sin(tick * 1.17) * 0.68 => rcNPan.pan;
```

## Energetic Leads And Machines

### 43. Squarepusher Bass Stabs

```chuck
// wf::declaration
SqrOsc qsNOsc => LPF qsNLpf => Gain qsNGain => Pan2 qsNPan => master;
0.0 => qsNGain.gain;
0.9 => qsNLpf.Q;
0.0 => float qsNEnv;
82.41 => float qsNFreq;

// wf::control
if (didTick == 1)
{
    tick % 16 => int qsNStep;
    if (qsNStep == 0) 82.41 => qsNFreq;
    else if (qsNStep == 2) 110.0 => qsNFreq;
    else if (qsNStep == 5) 73.42 => qsNFreq;
    else if (qsNStep == 9) 146.83 => qsNFreq;
    else if (qsNStep == 13) 98.0 => qsNFreq;
    if (qsNStep == 0 || qsNStep == 2 || qsNStep == 5 || qsNStep == 9 || qsNStep == 13)
        1.0 => qsNEnv;
}

qsNEnv * 0.69 => qsNEnv;
qsNFreq => qsNOsc.freq;
280.0 + qsNEnv * 2200.0 + bright * 1500.0 => qsNLpf.freq;
laneActiveN * (0.14 + intensity * 0.24) * qsNEnv => qsNGain.gain;
0.0 => qsNPan.pan;
```

### 44. Aphex Bell Machine

```chuck
// wf::declaration
SinOsc axNMod => SinOsc axNCar => Gain axNGain => Pan2 axNPan => master;
0.0 => axNGain.gain;
0.0 => float axNEnv;
440.0 => float axNFreq;

// wf::control
if (didTick == 1)
{
    ((tick * 5) + (tick / 3)) % 12 => int axNStep;
    220.0 * Math.pow(2.0, (axNStep * 2.0 + 12.0) / 12.0) => axNFreq;
    if (tick % 3 != 1)
        1.0 => axNEnv;
}

axNEnv * 0.86 => axNEnv;
axNFreq * (1.5 + bright * 1.5) => axNMod.freq;
axNFreq + (axNMod.last() * (110.0 + intensity * 180.0) * axNEnv) => axNCar.freq;
laneActiveN * (0.04 + bright * 0.08) * axNEnv => axNGain.gain;
Math.sin(tick * 0.23) * 0.62 => axNPan.pan;
```

### 45. Hyper Saw Siren

```chuck
// wf::declaration
SawOsc hsNA => Gain hsNMix => LPF hsNLpf => Gain hsNGain => Pan2 hsNPan => master;
SawOsc hsNB => hsNMix;
0.34 => hsNMix.gain;
0.0 => hsNGain.gain;
0.0 => float hsNLevel;

// wf::control
hsNLevel + (((laneActiveN * (0.08 + intensity * 0.08)) - hsNLevel) * 0.018) => hsNLevel;
220.0 * (1.0 + stepPhase * 0.50 + Math.sin(orbit * 6.28318) * 0.04) => hsNA.freq;
220.0 * (1.01 + stepPhase * 0.47) => hsNB.freq;
600.0 + bright * 2600.0 + stepPhase * 900.0 => hsNLpf.freq;
hsNLevel => hsNGain.gain;
Math.sin(orbit * 6.28318) * 0.28 => hsNPan.pan;
```

### 46. Broken FM Lead

```chuck
// wf::declaration
SinOsc bfNMod => SinOsc bfNCar => Gain bfNGain => Pan2 bfNPan => master;
0.0 => bfNGain.gain;
0.0 => float bfNEnv;
330.0 => float bfNFreq;

// wf::control
if (didTick == 1)
{
    tick % 10 => int bfNStep;
    if (bfNStep == 0) 329.63 => bfNFreq;
    else if (bfNStep == 1) 493.88 => bfNFreq;
    else if (bfNStep == 4) 392.00 => bfNFreq;
    else if (bfNStep == 7) 659.25 => bfNFreq;
    1.0 => bfNEnv;
}

bfNEnv * 0.56 => bfNEnv;
bfNFreq * (1.0 + ((tick % 4) * 0.125)) => bfNMod.freq;
bfNFreq + bfNMod.last() * (60.0 + bright * 200.0) => bfNCar.freq;
laneActiveN * (0.06 + intensity * 0.09) * bfNEnv => bfNGain.gain;
-0.34 => bfNPan.pan;
```

### 47. Laser Riser

```chuck
// wf::declaration
SqrOsc lrNOsc => Gain lrNGain => Pan2 lrNPan => master;
0.0 => lrNGain.gain;
0.0 => float lrNLevel;

// wf::control
lrNLevel + (((laneActiveN * (0.05 + bright * 0.05)) - lrNLevel) * 0.040) => lrNLevel;
330.0 + (stepPhase * stepPhase * 1600.0) + (orbit * 300.0) => lrNOsc.freq;
lrNLevel * (0.5 + stepPhase * 0.5) => lrNGain.gain;
Math.sin(stepPhase * 6.28318) * 0.60 => lrNPan.pan;
```

### 48. Robot Vowel Pulse

```chuck
// wf::declaration
SawOsc rvNOsc => BPF rvNBpf => Gain rvNGain => Pan2 rvNPan => master;
0.0 => rvNGain.gain;
7.0 => rvNBpf.Q;
0.0 => float rvNEnv;

// wf::control
if (didTick == 1)
{
    tick % 8 => int rvNStep;
    if (rvNStep == 0 || rvNStep == 3 || rvNStep == 6)
        1.0 => rvNEnv;
}

rvNEnv * 0.78 => rvNEnv;
110.0 * (1.0 + ((tick / 8) % 4) * 0.25) => rvNOsc.freq;
520.0 + ((tick % 4) * 420.0) + bright * 900.0 => rvNBpf.freq;
laneActiveN * (0.10 + intensity * 0.12) * rvNEnv => rvNGain.gain;
0.18 => rvNPan.pan;
```

## Dub, Echo, And Space

### 49. Dub Chord Stab

```chuck
// wf::declaration
SqrOsc dcNA => Gain dcNMix => LPF dcNLpf => Gain dcNGain => Pan2 dcNPan => master;
SqrOsc dcNB => dcNMix;
SqrOsc dcNC => dcNMix;
0.20 => dcNMix.gain;
0.0 => dcNGain.gain;
0.0 => float dcNEnv;

// wf::control
if (didTick == 1)
{
    tick % 16 => int dcNStep;
    if (dcNStep == 0 || dcNStep == 9)
        1.0 => dcNEnv;
}

dcNEnv * 0.91 => dcNEnv;
196.00 => dcNA.freq;
246.94 * (1.0 + orbit * 0.004) => dcNB.freq;
293.66 => dcNC.freq;
380.0 + bright * 1100.0 + dcNEnv * 800.0 => dcNLpf.freq;
laneActiveN * (0.12 + intensity * 0.08) * dcNEnv => dcNGain.gain;
Math.sin(orbit * 6.28318) * 0.30 => dcNPan.pan;
```

### 50. Skank Organ

```chuck
// wf::declaration
TriOsc soNA => Gain soNMix => Gain soNGain => Pan2 soNPan => master;
TriOsc soNB => soNMix;
0.32 => soNMix.gain;
0.0 => soNGain.gain;
0.0 => float soNEnv;

// wf::control
if (didTick == 1)
{
    tick % 8 => int soNStep;
    if (soNStep == 2 || soNStep == 6)
        1.0 => soNEnv;
}

soNEnv * 0.72 => soNEnv;
261.63 => soNA.freq;
329.63 => soNB.freq;
laneActiveN * (0.12 + bright * 0.05) * soNEnv => soNGain.gain;
-0.18 => soNPan.pan;
```

### 51. Faux Tape Echo Ping

```chuck
// wf::declaration
SinOsc epNDirect => Gain epNDirectGain => Pan2 epNPan => master;
SinOsc epNEcho => Gain epNEchoGain => epNPan;
0.0 => epNDirectGain.gain;
0.0 => epNEchoGain.gain;
0.0 => float epNEnv;
0.0 => float epNEchoEnv;
660.0 => float epNFreq;

// wf::control
if (didTick == 1)
{
    tick % 12 => int epNStep;
    epNEchoEnv * 0.82 => epNEchoEnv;
    if (epNStep == 0 || epNStep == 7)
    {
        1.0 => epNEnv;
        0.65 => epNEchoEnv;
        if (epNStep == 0) 660.0 => epNFreq;
        else 880.0 => epNFreq;
    }
}

epNEnv * 0.55 => epNEnv;
epNFreq => epNDirect.freq;
epNFreq * 0.997 => epNEcho.freq;
laneActiveN * 0.08 * epNEnv => epNDirectGain.gain;
laneActiveN * (0.035 + bright * 0.035) * epNEchoEnv => epNEchoGain.gain;
Math.sin(orbit * 6.28318) * 0.45 => epNPan.pan;
```

### 52. Space Beacon

```chuck
// wf::declaration
SinOsc beNOsc => Gain beNGain => Pan2 beNPan => master;
0.0 => beNGain.gain;
0.0 => float beNEnv;

// wf::control
if (didTick == 1)
{
    tick % 24 => int beNStep;
    if (beNStep == 0 || beNStep == 13)
        1.0 => beNEnv;
}

beNEnv * 0.93 => beNEnv;
880.0 + Math.sin(orbit * 6.28318) * 120.0 => beNOsc.freq;
laneActiveN * (0.04 + bright * 0.05) * beNEnv => beNGain.gain;
Math.sin(orbit * 6.28318) * 0.72 => beNPan.pan;
```

### 53. Distant Fog Horn

```chuck
// wf::declaration
SinOsc fhNOsc => LPF fhNLpf => Gain fhNGain => Pan2 fhNPan => master;
0.0 => fhNGain.gain;
0.0 => float fhNLevel;

// wf::control
fhNLevel + (((laneActiveN * (0.035 + intensity * 0.030)) - fhNLevel) * 0.006) => fhNLevel;
73.42 * (1.0 + Math.sin(orbit * 6.28318) * 0.006) => fhNOsc.freq;
160.0 + bright * 260.0 => fhNLpf.freq;
fhNLevel => fhNGain.gain;
Math.sin(orbit * 6.28318) * 0.24 => fhNPan.pan;
```

### 54. High Space Dust

```chuck
// wf::declaration
Noise sdNNoise => HPF sdNHpf => Gain sdNGain => Pan2 sdNPan => master;
0.0 => sdNGain.gain;
0.0 => float sdNEnv;

// wf::control
if (didTick == 1)
{
    if (((tick * 19) % 31) < 4)
        1.0 => sdNEnv;
}

sdNEnv * 0.34 => sdNEnv;
7400.0 + bright * 3400.0 => sdNHpf.freq;
laneActiveN * (0.018 + bright * 0.035) * sdNEnv => sdNGain.gain;
Math.sin(tick * 0.73) * 0.80 => sdNPan.pan;
```

## Utility And Structure

### 55. Silent Timing Lane

Use this for a lane that exists only to show timing and phase in the UI.

```chuck
// wf::declaration
Gain stNSilent => master;
0.0 => stNSilent.gain;

// wf::control
0.0 => stNSilent.gain;
```

### 56. Click Reference

```chuck
// wf::declaration
TriOsc crNOsc => Gain crNGain => Pan2 crNPan => master;
0.0 => crNGain.gain;
0.0 => float crNEnv;

// wf::control
if (didTick == 1)
{
    if (tick % 4 == 0)
        1.0 => crNEnv;
    else
        0.45 => crNEnv;
}

crNEnv * 0.26 => crNEnv;
if (tick % 4 == 0) 1400.0 => crNOsc.freq;
else 900.0 => crNOsc.freq;
laneActiveN * 0.055 * crNEnv => crNGain.gain;
0.0 => crNPan.pan;
```

### 57. Sidechain Ghost Duck

This is an audible ducking pulse for testing how other lanes sit against a four-on-the-floor shape.

```chuck
// wf::declaration
SinOsc gdNSub => Gain gdNGain => Pan2 gdNPan => master;
0.0 => gdNGain.gain;
0.0 => float gdNEnv;

// wf::control
if (didTick == 1 && tick % 4 == 0)
    1.0 => gdNEnv;

gdNEnv * 0.86 => gdNEnv;
38.0 + gdNEnv * 30.0 => gdNSub.freq;
laneActiveN * 0.12 * gdNEnv => gdNGain.gain;
0.0 => gdNPan.pan;
```

### 58. Bar Marker Tone

```chuck
// wf::declaration
SinOsc bmNOsc => Gain bmNGain => Pan2 bmNPan => master;
0.0 => bmNGain.gain;
0.0 => float bmNEnv;

// wf::control
if (didTick == 1 && tick % 16 == 0)
    1.0 => bmNEnv;

bmNEnv * 0.76 => bmNEnv;
523.25 => bmNOsc.freq;
laneActiveN * 0.08 * bmNEnv => bmNGain.gain;
0.0 => bmNPan.pan;
```

### 59. Fade-In Utility Drone

```chuck
// wf::declaration
SinOsc fiNOsc => Gain fiNGain => Pan2 fiNPan => master;
0.0 => fiNGain.gain;
0.0 => float fiNLevel;

// wf::control
fiNLevel + (((laneActiveN * (0.10 + bright * 0.03)) - fiNLevel) * 0.004) => fiNLevel;
110.0 * (1.0 + Math.sin(orbit * 6.28318) * 0.004) => fiNOsc.freq;
fiNLevel => fiNGain.gain;
0.0 => fiNPan.pan;
```

### 60. Fade-Out Tail Tester

```chuck
// wf::declaration
TriOsc ftNOsc => Gain ftNGain => Pan2 ftNPan => master;
0.0 => ftNGain.gain;
0.0 => float ftNEnv;

// wf::control
if (didTick == 1 && tick % 8 == 0)
    1.0 => ftNEnv;

ftNEnv * 0.965 => ftNEnv;
330.0 => ftNOsc.freq;
laneActiveN * 0.10 * ftNEnv => ftNGain.gain;
0.0 => ftNPan.pan;
```

## Quick Variations

These are small edits that can be applied to many recipes:

- Make a lane softer: multiply the final gain by `0.5`.
- Make a lane more aggressive: add `+ intensity * 0.12` to the final gain expression.
- Make a lane brighter: increase any filter frequency expression that uses `bright`.
- Make a lane drift: multiply frequency by `(1.0 + Math.sin(orbit * 6.28318) * 0.01)`.
- Make a lane pulse less often: replace `tick % 8` with `tick % 16`.
- Make a lane busier: add extra trigger steps inside the `didTick` block.
- Make a lane wider: set pan with `Math.sin(orbit * 6.28318) * 0.5`.
- Make a lane delayed in time: use the phase offset control in focused track view rather than adding silence in code.

## Useful Frequencies

- Low kick body: `34.0` to `55.0`
- Sub bass: `43.65`, `49.0`, `55.0`, `65.41`
- Bass roots: `73.42`, `82.41`, `98.0`, `110.0`
- Middle C area: `261.63`, `293.66`, `329.63`, `392.0`
- Bright lead: `523.25`, `659.25`, `783.99`, `880.0`
- Air/noise filter: `2800.0` to `9000.0`
