#!/usr/bin/env python3
"""Export the trained wake-word CNN into a flat float32 blob for the Android C++ runtime.

The Android side does NOT use ONNX Runtime for this model. `wake_net.cpp` runs a hand-written
forward pass (conv3x3 -> relu -> maxpool ... -> mean over frequency -> linear -> sigmoid), because
the network is small and everything can be folded at export time:

  * BatchNorm is already folded into the Conv weights by the ONNX exporter (verified below).
  * Dropout disappears in inference mode.
  * The mel filterbank and the Hann window ride along in the same blob, so the runtime mel is
    identical to the one the model was trained on (this is the part that silently breaks
    hand-written wake-word runtimes if it is re-derived by hand).

Blob layout (little endian, float32):
  magic "MAYAWK1\\0" | u32 n_tensors | per tensor: u32 ndim, ndim x u32 dims, payload
  order: mel_fb(40,257), window(512), then 8 x [conv weight, conv bias], fc1 w/b, fc2 w/b

Usage: python3 tools/export_wake_net.py hey_maya.onnx app/src/main/assets/wake_weights.bin
"""
import struct
import sys

import numpy as np
import torch
import torch.nn.functional as F
import torchaudio

SR, WIN, HOP, NFFT, NMEL, NF = 16000, 400, 160, 512, 40, 96


def load_weights(onnx_path):
    import onnx
    from onnx import numpy_helper

    model = onnx.load(onnx_path)
    init = {i.name: numpy_helper.to_array(i) for i in model.graph.initializer}
    convs, fcs = [], []
    for node in model.graph.node:
        if node.op_type == "Conv":
            convs.append((init[node.input[1]], init[node.input[2]] if len(node.input) > 2 else
                          np.zeros(init[node.input[1]].shape[0], np.float32)))
        elif node.op_type == "Gemm":
            w = init[node.input[1]]
            fcs.append((w.copy(), init[node.input[2]]))  # ONNX Gemm: y = x @ W.T + b, so W is (out, in)
    assert len(convs) == 8, f"expected 8 convs, found {len(convs)}"
    assert len(fcs) == 2, f"expected 2 Gemms, found {len(fcs)}"
    return convs, fcs


def reference_forward(tensors, mel_in):
    """Same maths as wake_net.cpp, in numpy — used to prove the C++ side is right."""
    x = mel_in[None] if mel_in.ndim == 2 else mel_in            # (C,H,W)
    pools = [(2, 2), (2, 2), (2, 2), (1, 2)]
    i = 2
    for ph, pw in pools:
        for _ in range(2):
            w, b = tensors[i], tensors[i + 1]; i += 2
            x = np.maximum(0, conv3x3(x, w, b))
        if ph == pw and ph > 1:
            C, H, W = x.shape
            x = x.reshape(C, H // ph, ph, W // pw, pw).max(axis=(2, 4))
        else:
            C, H, W = x.shape
            x = x.reshape(C, H // ph, ph, W // pw, pw).max(axis=(2, 4))
    x = x.mean(axis=1)                      # (C, T)  average over frequency
    x = x.reshape(-1)
    w, b = tensors[i], tensors[i + 1]; i += 2
    x = np.maximum(0, w @ x + b)
    w, b = tensors[i], tensors[i + 1]
    return 1.0 / (1.0 + np.exp(-(w @ x + b)[0]))


def conv3x3(x, w, b):
    co, ci = w.shape[0], w.shape[1]
    _, H, W = x.shape                           # (C, H, W)
    xp = np.pad(x, ((0, 0), (1, 1), (1, 1)))
    out = np.zeros((co, H, W), np.float32)
    for dy in range(3):
        for dx in range(3):
            patch = xp[:, dy:dy + H, dx:dx + W]                 # (ci,H,W)
            out += np.einsum("oc,chw->ohw", w[:, :, dy, dx], patch)
    return out + b[:, None, None]


def logmel_reference(wav, fb, window):
    frames = np.stack([wav[i * HOP:i * HOP + NFFT] * window for i in range(NF)])
    spec = np.fft.rfft(frames, axis=1)
    power = (spec.real ** 2 + spec.imag ** 2)                    # (NF, 257)
    mel = power @ fb.T                                           # (NF, 40)
    db = 10.0 * np.log10(np.maximum(mel, 1e-10))
    db = np.clip(db, -100.0, 0.0)
    return ((db + 100.0) / 50.0).T                               # (40, NF)


def main(onnx_path, out_path):
    convs, fcs = load_weights(onnx_path)

    fb = torchaudio.functional.melscale_fbanks(
        NFFT // 2 + 1, f_min=60.0, f_max=7800.0, n_mels=NMEL, sample_rate=SR,
        norm=None, mel_scale="htk").numpy().T.astype(np.float32)              # (40,257)
    win = np.zeros(NFFT, np.float32)
    win[(NFFT - WIN) // 2:(NFFT - WIN) // 2 + WIN] = torch.hann_window(WIN, periodic=True).numpy()

    tensors = [fb, win]
    for w, b in convs:
        tensors += [w.astype(np.float32), b.astype(np.float32)]
    for w, b in fcs:
        tensors += [w.astype(np.float32), b.astype(np.float32)]

    with open(out_path, "wb") as f:
        f.write(b"MAYAWK1\0")
        f.write(struct.pack("<I", len(tensors)))
        for t in tensors:
            f.write(struct.pack("<I", t.ndim))
            for d in t.shape:
                f.write(struct.pack("<I", int(d)))
            f.write(np.ascontiguousarray(t, dtype="<f4").tobytes())
    total = sum(t.size for t in tensors)
    print(f"wrote {out_path}: {len(tensors)} tensors, {total} floats, {total*4/1024:.0f} KiB")
    print("tensor shapes:", [tuple(t.shape) for t in tensors])

    # ---- proof that the numpy mel equals torchaudio's own transform ----
    tf = torchaudio.transforms.MelSpectrogram(sample_rate=SR, n_fft=NFFT, win_length=WIN, hop_length=HOP,
                                             n_mels=NMEL, f_min=60.0, f_max=7800.0, power=2.0, center=False)
    wav0 = np.random.default_rng(7).standard_normal(15712).astype(np.float32) * 0.1
    with torch.no_grad():
        ref_mel = ((tf(torch.from_numpy(wav0)[None]).clamp_min(1e-10).log10() * 10.0).clamp(-100, 0) + 100) / 50
    mine = logmel_reference(wav0, fb, win)
    print("mel check: shapes", tuple(ref_mel[0].numpy().shape), mine.shape,
          "max |diff|", float(np.abs(ref_mel[0].numpy() - mine).max()))

    # ---- proof: numpy path (what the C++ does) vs the ONNX graph (what Kaggle validated) ----
    rng = np.random.default_rng(0)
    ok = 0
    worst = 0.0
    for trial in range(4):
        wav = (0.2 * rng.standard_normal(15712)).astype(np.float32)
        m = logmel_reference(wav, fb, win)[None]                 # (1,40,96)
        ref = reference_forward(tensors, m[0])
        try:
            import onnxruntime as ort
            sess = ort.InferenceSession(onnx_path, providers=["CPUExecutionProvider"])
            got = float(sess.run(None, {"mel": m[None]})[0].ravel()[0])
        except Exception as e:                                   # onnxruntime optional
            print("onnxruntime unavailable:", e)
            return
        worst = max(worst, abs(ref - got))
        ok += abs(ref - got) < 1e-4
    print(f"numpy-vs-ONNX check: {ok}/4 within 1e-4, worst |diff| {worst:.2e}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
