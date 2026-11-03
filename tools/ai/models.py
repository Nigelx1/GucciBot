"""The AI's three networks in PyTorch, for training only (contract s5, GBNN v1).

    Pilot   MLP, obs -> 256 -> 256 -> 1 logit: hold this tick if sigmoid >= 0.5
    Critic  MLP, obs -> 256 -> 256 -> 2 sigmoid heads: still alive in 1 s if it
            holds now / if it releases now
    Reader  conv1d stack over the level tape: per column [y_rel, 8 mode logits,
            gravity logit, hold_frac]

Only what a GBNN v1 file can carry, because the mod runs these as plain C++
loops: dense and conv1d layers (same padding, dilation), activations
none/relu/sigmoid/tanh, and the input normalization, which is the mean/std in
the file's header (the network sees (x-mean)/std). No batch or layer norm.

Each model takes RAW inputs and normalizes them with its in_mean/in_std
buffers, so what trains is exactly what exports. forward() gives what the
loss needs (logits); export_forward() gives what the GBNN file computes (the
critic's sigmoid included). gbnn_layers() lists the layers for export.py.

The conv1d semantics match the contract's C++ loop:
    out[o][t] = b[o] + sum_i sum_k w[o][i][k] * x[i][t + (k-(K-1)/2)*dilation]
with x zero outside [0, C), at every layer (PyTorch's zero padding of
(K-1)/2*dilation on both sides is the same thing).
"""
import json
import math
import os
import time

import numpy as np
import torch
from torch import nn

import gbai

READER_OUTPUTS = (["y_rel"] + [f"mode_logit_{m}" for m in gbai.MODES] + ["gravity_logit", "hold_frac"])
READER_Y, READER_MODE0, READER_GRAVITY, READER_HOLD = 0, 1, 1 + len(gbai.MODES), 2 + len(gbai.MODES)


class _Net(nn.Module):
    """Holds the input normalization as buffers (saved with the weights)."""

    model_kind = ""
    gbnn_kind = gbai.KIND_MLP
    out_names = []

    def __init__(self, n_in):
        super().__init__()
        self.n_in = int(n_in)
        self.register_buffer("in_mean", torch.zeros(self.n_in))
        self.register_buffer("in_std", torch.ones(self.n_in))

    def set_norm(self, mean, std):
        """The training split's per-input mean and std (std 1 where an input never changes)."""
        mean = np.asarray(mean, np.float32).reshape(self.n_in)
        std = np.asarray(std, np.float32).reshape(self.n_in)
        std = np.where(std > 1e-6, std, 1.0).astype(np.float32)
        with torch.no_grad():
            self.in_mean.copy_(torch.from_numpy(mean))
            self.in_std.copy_(torch.from_numpy(std))


class _MLP(_Net):
    def __init__(self, n_in, hidden, n_out, out_activation):
        super().__init__(n_in)
        self.hidden = [int(h) for h in hidden]
        dims = [self.n_in, *self.hidden, int(n_out)]
        self.layers = nn.ModuleList(nn.Linear(a, b) for a, b in zip(dims[:-1], dims[1:]))
        self.out_activation = out_activation

    def logits(self, x):
        """Raw inputs [B, n_in] -> the last layer's output before its activation [B, n_out]."""
        h = (x - self.in_mean) / self.in_std
        for lin in self.layers[:-1]:
            h = torch.relu(lin(h))
        return self.layers[-1](h)

    def export_forward(self, x):
        """What the GBNN file computes: [B, n_out]."""
        out = self.logits(x)
        if self.out_activation == gbai.ACT_SIGMOID:
            return torch.sigmoid(out)
        if self.out_activation == gbai.ACT_TANH:
            return torch.tanh(out)
        if self.out_activation == gbai.ACT_RELU:
            return torch.relu(out)
        return out

    def gbnn_layers(self):
        out = []
        for i, lin in enumerate(self.layers):
            last = i == len(self.layers) - 1
            w = lin.weight.detach().cpu().numpy().astype(np.float32)
            out.append(gbai.Layer(gbai.LAYER_DENSE, w.shape[1], w.shape[0], 1, 1,
                                  self.out_activation if last else gbai.ACT_RELU, w[:, :, None],
                                  lin.bias.detach().cpu().numpy()))
        return out


class Pilot(_MLP):
    """P(hold) each tick from what the pilot sees. forward() -> logits [B]."""

    model_kind = "pilot"
    out_names = ["hold_logit"]

    def __init__(self, n_in, hidden=(256, 256)):
        super().__init__(n_in, hidden, 1, gbai.ACT_NONE)

    def forward(self, x):
        return self.logits(x)[..., 0]

    def config(self):
        return {"kind": "pilot", "n_in": self.n_in, "hidden": list(self.hidden)}


class Critic(_MLP):
    """P(still alive in 1 s) if it holds / if it releases now. forward() -> logits [B, 2]
    (heads in out_names order); the GBNN file ends in a sigmoid."""

    model_kind = "critic"
    out_names = ["alive_hold", "alive_release"]
    HOLD, RELEASE = 0, 1

    def __init__(self, n_in, hidden=(256, 256)):
        super().__init__(n_in, hidden, 2, gbai.ACT_SIGMOID)

    def forward(self, x):
        return self.logits(x)

    def config(self):
        return {"kind": "critic", "n_in": self.n_in, "hidden": list(self.hidden)}


class Reader(_Net):
    """The level reader: conv1d over the tape's columns.

    Input [B, K channels, C columns] (the tape file is [C][K]: transpose it).
    Output [B, 11, C]: y_rel (blocks above the tape's y0), 8 mode logits in the
    meta's mode order, the gravity-flipped logit, and hold_frac (a plain
    number trained toward 0..1; the file's last layer has one activation for
    all outputs, so the mod clamps it).
    """

    model_kind = "reader"
    gbnn_kind = gbai.KIND_CONV
    out_names = list(READER_OUTPUTS)

    def __init__(self, n_in, channels=64, kernel=5, dilations=(1, 2, 4, 8, 16, 32)):
        super().__init__(n_in)
        if kernel < 1 or kernel % 2 == 0:
            raise ValueError(f"the reader's kernel must be odd, not {kernel}")
        self.channels = int(channels)
        self.kernel = int(kernel)
        self.dilations = [int(d) for d in dilations]
        convs = []
        width = self.n_in
        for d in self.dilations:
            convs.append(nn.Conv1d(width, self.channels, self.kernel, dilation=d, padding=(self.kernel - 1) // 2 * d))
            width = self.channels
        self.convs = nn.ModuleList(convs)
        self.head = nn.Conv1d(width, len(READER_OUTPUTS), 1)

    def forward(self, x):
        h = (x - self.in_mean[:, None]) / self.in_std[:, None]
        for conv in self.convs:
            h = torch.relu(conv(h))
        return self.head(h)

    def export_forward(self, x):
        return self.forward(x)

    def receptive_field(self):
        """Columns one output column sees (centered)."""
        return 1 + (self.kernel - 1) * sum(self.dilations)

    def gbnn_layers(self):
        out = []
        for conv, d in zip(self.convs, self.dilations):
            out.append(gbai.Layer(gbai.LAYER_CONV1D, conv.in_channels, conv.out_channels, self.kernel, d,
                                  gbai.ACT_RELU, conv.weight.detach().cpu().numpy(), conv.bias.detach().cpu().numpy()))
        h = self.head
        out.append(gbai.Layer(gbai.LAYER_CONV1D, h.in_channels, h.out_channels, 1, 1, gbai.ACT_NONE,
                              h.weight.detach().cpu().numpy(), h.bias.detach().cpu().numpy()))
        return out

    def config(self):
        return {"kind": "reader", "n_in": self.n_in, "channels": self.channels, "kernel": self.kernel,
                "dilations": list(self.dilations)}


KINDS = {"pilot": Pilot, "critic": Critic, "reader": Reader}


def build(config):
    """A fresh model from config() output."""
    cfg = dict(config)
    kind = cfg.pop("kind")
    if kind == "reader":
        return Reader(cfg["n_in"], cfg["channels"], cfg["kernel"], cfg["dilations"])
    return KINDS[kind](cfg["n_in"], cfg["hidden"])


def parse_ints(text):
    """'256,256' -> [256, 256]."""
    return [int(v) for v in str(text).replace(" ", "").split(",") if v]


# ---- checkpoints -----------------------------------------------------------------

def save_checkpoint(path, obj):
    """torch.save under a temporary name, then renamed into place (a crash never leaves half a file)."""
    tmp = f"{path}.{os.getpid()}.tmp"
    torch.save(obj, tmp)
    gbai._replace(tmp, path)


def load_checkpoint(path):
    """A checkpoint saved by the trainers (tensors and plain Python values only)."""
    return torch.load(path, map_location="cpu", weights_only=True)


def from_checkpoint(path_or_ck):
    """(model with its weights, checkpoint dict) from a trainer's .pt file."""
    ck = load_checkpoint(path_or_ck) if isinstance(path_or_ck, str) else path_or_ck
    model = build(ck["model_config"])
    model.load_state_dict(ck["model_state"])
    model.eval()
    return model, ck


def set_threads(n):
    """CPU threads for torch AND numpy's own BLAS (Nigel's PC: 6 by default, one per core, so the
    PC stays usable). torch.set_num_threads alone leaves numpy at 12 (gbai.limit_blas_threads)."""
    n = max(1, int(n))
    torch.set_num_threads(n)
    gbai.limit_blas_threads(n)
    return n


def set_seed(seed):
    """Seeds torch's weight init. The data order was already seeded (numpy, by --seed and the
    epoch); without this the same data, settings and --seed trained a different network every
    run. Same --threads too: CPU math can round differently with another thread count."""
    torch.manual_seed(int(seed))


# ---- the epoch loop the pilot and the critic share ---------------------------------

def lr_at(step, total, base):
    """Linear warmup, then cosine down to 5% of base (a plain float: checkpoints hold no numpy values)."""
    warm = max(1, min(300, total // 10))
    if step < warm:
        return float(base * (step + 1) / warm)
    p = min(1.0, (step - warm) / max(1, total - warm))
    return float(base * (0.05 + 0.95 * 0.5 * (1.0 + math.cos(math.pi * p))))


def plain(obj):
    """obj as plain JSON values (numpy numbers become Python ones): torch.load(weights_only=True) takes only those."""
    return json.loads(json.dumps(obj, default=gbai._json_default))


def fit_rows(model, X, Y, W, settings, validate, out, signature, layout_hash, prog, log=print, near=None):
    """Trains an MLP on rows: the loop the pilot and the critic share.

    X float16 [N][n_in] raw inputs; Y float32 [N][k] targets; W float32 [N][k]
    loss weights (0 where a row has no label for that output); the loss is
    binary cross-entropy on model.logits(x) [B][k], weighted, summed over k.
    settings (an argparse namespace works): epochs, batch, lr, weight_decay,
    seed, far_keep, select ("timing" / "loss" / "last"), fresh, ema. near: bool
    [N], the rows every epoch takes when far_keep < 1 (the others are sampled
    at far_keep and weigh 1/far_keep).

    ema: what gets validated, checkpointed as model_state and exported is an
    exponential moving average of the weights over about `ema` steps (-1:
    half an epoch, 50..2000 steps; 0: off). A press is a boundary 1 tick wide,
    and every noisy step moves it; the average holds it still. (On the toy
    data the held-out timing of the raw weights swung by 20 points between
    identical runs.) The raw weights ride along as train_state for resuming.

    validate(model) -> (val_loss, val_metrics, score, text): score picks the
    epoch under "timing" (None falls back to the loss); text goes in the log.
    Pass validate=None when there's nothing to validate on (the last epoch is
    kept).

    Every epoch: last.pt (and best.pt when the pick improves) in `out`, and
    the progress file. With a last.pt whose signature matches, it carries on
    after that epoch; one that doesn't match stops it (settings.fresh starts
    over). Leaves the picked epoch's weights in the model; returns (best, history).
    """
    import torch.nn.functional as F
    last_path, best_path = os.path.join(out, "last.pt"), os.path.join(out, "best.pt")
    opt = torch.optim.AdamW(model.parameters(), lr=settings.lr, weight_decay=settings.weight_decay)
    start_epoch, step, history = 0, 0, []
    best = {"epoch": None, "val_loss": None, "score": None}
    n = len(X)
    far_keep = getattr(settings, "far_keep", 1.0)
    if near is None or far_keep >= 1:
        per_epoch = n
    else:
        per_epoch = int(near.sum() + far_keep * (~near).sum())
    steps_per_epoch = max(1, int(np.ceil(per_epoch / settings.batch)))
    total_steps = steps_per_epoch * settings.epochs
    ema_steps = getattr(settings, "ema", -1)
    if ema_steps is None or ema_steps < 0:
        ema_steps = int(min(2000, max(50, steps_per_epoch // 2)))
    signature = dict(signature, ema_steps=ema_steps)
    ema = {k: v.detach().clone() for k, v in model.state_dict().items()} if ema_steps > 0 else None
    if os.path.exists(last_path) and not settings.fresh:
        ck = load_checkpoint(last_path)
        want, have = plain(signature), ck.get("signature") or {}
        if have != want:
            differ = sorted(k for k in set(want) | set(have) if want.get(k) != have.get(k))
            raise SystemExit(f"{last_path} was trained with other data or settings ({', '.join(differ)}); run with "
                             "--fresh to start over (or give --out another folder)")
        model.load_state_dict(ck.get("train_state") or ck["model_state"])
        if ema is not None:
            ema = {k: v.detach().clone() for k, v in ck["model_state"].items()}
        opt.load_state_dict(ck["optimizer_state"])
        start_epoch, step, history, best = ck["epoch"], ck["step"], ck["history"], ck["best"]
        log(f"resuming after epoch {start_epoch} (step {step})")

    prog.set(state="training", epochs=settings.epochs, epoch=start_epoch, steps_per_epoch=steps_per_epoch, step=step,
             history=history, best_epoch=best["epoch"], force=True, message="")
    session_steps, session_time, val_time = 0, 0.0, []
    for epoch in range(start_epoch, settings.epochs):
        rng = np.random.default_rng([settings.seed, epoch])
        if near is None or far_keep >= 1:
            idx = rng.permutation(n)
            boost = None
        else:
            idx = np.flatnonzero(near | (rng.random(n) < far_keep))
            idx = idx[rng.permutation(len(idx))]
            boost = np.where(near, 1.0, 1.0 / far_keep).astype(np.float32)[:, None]
        n_steps = int(np.ceil(len(idx) / settings.batch))
        model.train()
        t_epoch = time.time()
        loss_avg = None
        for b in range(n_steps):
            t_step = time.time()
            bi = np.sort(idx[b * settings.batch:(b + 1) * settings.batch])
            xb = torch.from_numpy(X[bi].astype(np.float32))
            yb = torch.from_numpy(Y[bi])
            wb = W[bi] if boost is None else W[bi] * boost[bi]
            lr = lr_at(step, total_steps, settings.lr)
            for g in opt.param_groups:
                g["lr"] = lr
            loss = (F.binary_cross_entropy_with_logits(model.logits(xb), yb, reduction="none")
                    * torch.from_numpy(wb)).sum(1).mean()
            opt.zero_grad(set_to_none=True)
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 5.0)
            opt.step()
            step += 1
            if ema is not None:
                d = min(1.0 - 1.0 / ema_steps, (1.0 + step) / (10.0 + step))
                with torch.no_grad():
                    for k, v in model.state_dict().items():
                        if v.dtype.is_floating_point:
                            ema[k].mul_(d).add_(v, alpha=1.0 - d)
                        else:
                            ema[k].copy_(v)
            lv = loss.item()
            loss_avg = lv if loss_avg is None else 0.98 * loss_avg + 0.02 * lv
            session_steps += 1
            session_time += time.time() - t_step
            left = (settings.epochs - epoch) * steps_per_epoch - (b + 1)
            eta = left * session_time / session_steps + (settings.epochs - epoch) * (np.mean(val_time) if val_time
                                                                                     else 0.0)
            prog.set(epoch=epoch, step=step, train_loss=loss_avg, eta_seconds=eta, lr=lr)
        train_seconds = time.time() - t_epoch
        t_val = time.time()
        raw = None
        if ema is not None:  # validate, keep and export the average; train on with the raw weights
            raw = {k: v.detach().clone() for k, v in model.state_dict().items()}
            model.load_state_dict(ema)
        model.eval()
        val_loss, val, score, text = validate(model) if validate else (None, None, None, "")
        val_time.append(time.time() - t_val)
        entry = {"epoch": epoch + 1, "train_loss": loss_avg, "val_loss": val_loss, "score": score,
                 "seconds": round(train_seconds, 1), "val": val}
        history.append(entry)
        if settings.select == "last" or validate is None:
            improved = True
        elif settings.select == "loss" or score is None:
            improved = val_loss is not None and (best["val_loss"] is None or val_loss < best["val_loss"])
        else:
            improved = best["score"] is None or score > best["score"]
        if improved:
            best = {"epoch": epoch + 1, "val_loss": val_loss, "score": score}
        history, best = plain(history), plain(best)
        ck = {"format": "gbai-ckpt", "model": model.model_kind, "model_config": model.config(),
              "model_state": model.state_dict(), "optimizer_state": opt.state_dict(), "epoch": epoch + 1,
              "step": step, "signature": plain(signature), "layout_hash": layout_hash, "history": history,
              "best": best}
        if raw is not None:
            ck["train_state"] = raw
        save_checkpoint(last_path, ck)
        if improved:
            save_checkpoint(best_path, ck)
        if raw is not None:
            model.load_state_dict(raw)
        left = (settings.epochs - epoch - 1) * (steps_per_epoch * session_time / max(1, session_steps)
                                                + float(np.mean(val_time)))
        prog.set(force=True, epoch=epoch + 1, step=step, train_loss=loss_avg, val_loss=val_loss, val=val,
                 history=history, best_epoch=best["epoch"], eta_seconds=left)
        log(f"epoch {epoch + 1}/{settings.epochs}: train {loss_avg:.4f}" +
            (f", val {val_loss:.4f}" if val_loss is not None else "") + (f", {text}" if text else "") +
            f" ({train_seconds:.0f} s)")
    pick = best_path if best["epoch"] is not None and os.path.exists(best_path) else last_path
    if os.path.exists(pick):
        model.load_state_dict(load_checkpoint(pick)["model_state"])
    model.eval()
    return best, history
