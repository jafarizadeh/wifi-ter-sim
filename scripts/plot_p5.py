#!/usr/bin/env python3
import os, glob, argparse
import pandas as pd
import matplotlib.pyplot as plt

def ensure_dir(p): os.makedirs(p, exist_ok=True)
def to_num(x): return pd.to_numeric(x, errors="coerce")

def load_summary(path):
    df = pd.read_csv(path)
    df.columns = [c.strip() for c in df.columns]
    if "distance" not in df.columns and "effectiveDistance" in df.columns:
        df = df.rename(columns={"effectiveDistance":"distance"})

    req = ["transport","nSta","run","sumGoodputbps","jain"]
    for c in req:
        if c not in df.columns:
            raise ValueError(f"missing column in summary: {c}")

    df["transport"] = df["transport"].astype(str).str.strip().str.lower()
    df["nSta"] = to_num(df["nSta"]).astype("Int64")
    df["run"] = to_num(df["run"]).astype("Int64")
    df["sumGoodputbps"] = to_num(df["sumGoodputbps"])
    df["jain"] = to_num(df["jain"])
    df = df.dropna(subset=req)
    df["nSta"] = df["nSta"].astype(int)
    df["run"] = df["run"].astype(int)
    return df

def parse_persta_name(fp):
    # persta_{transport}_n{N}_run{R}.csv
    base = os.path.basename(fp).replace(".csv","")
    parts = base.split("_")
    tr = parts[1].lower()
    n = r = None
    for t in parts:
        if t.startswith("n") and t[1:].isdigit(): n = int(t[1:])
        if t.startswith("run") and t[3:].isdigit(): r = int(t[3:])
    return tr, n, r

def load_persta(raw_dir):
    files = sorted(glob.glob(os.path.join(raw_dir,"persta_*.csv")))
    if not files:
        raise FileNotFoundError(f"no persta_*.csv in {raw_dir}")
    rows = []
    for fp in files:
        tr,n,r = parse_persta_name(fp)
        d = pd.read_csv(fp)
        d.columns = [c.strip() for c in d.columns]
        if "staId" not in d.columns or "goodputbps" not in d.columns:
            continue
        d["transport"]=tr; d["nSta"]=n; d["run"]=r
        d["staId"]=to_num(d["staId"]).fillna(0).astype(int)
        d["goodputbps"]=to_num(d["goodputbps"]).fillna(0.0)
        rows.append(d)
    if not rows:
        raise RuntimeError("persta files exist but no usable staId/goodputbps found")
    out = pd.concat(rows, ignore_index=True)
    out = out.dropna(subset=["transport","nSta","run"])
    out["transport"] = out["transport"].astype(str).str.strip().str.lower()
    out["nSta"] = out["nSta"].astype(int)
    out["run"] = out["run"].astype(int)
    return out

def savefig(p):
    plt.tight_layout()
    plt.savefig(p, dpi=170)
    plt.close()

def plot_sum_goodput(summary, outdir):
    agg = (summary.groupby(["transport","nSta"], as_index=False)
                 .agg(sumGoodputbps=("sumGoodputbps","mean")))
    # sanity print: این باید با جدول summary شما یکی باشد
    print("\n[Sanity] Sum goodput used (Mbps):")
    tmp = agg.copy()
    tmp["sumGoodputMbps"] = tmp["sumGoodputbps"]/1e6
    print(tmp.sort_values(["transport","nSta"])[["transport","nSta","sumGoodputMbps"]].to_string(index=False))

    plt.figure()
    for tr in sorted(agg["transport"].unique()):
        d = agg[agg["transport"]==tr].sort_values("nSta")
        plt.plot(d["nSta"], d["sumGoodputbps"]/1e6, marker="o", label=tr.upper())
    plt.xlabel("Number of STAs (N)")
    plt.ylabel("Sum goodput (Mbps)")
    plt.title("Sum Goodput vs N")
    plt.legend()
    savefig(os.path.join(outdir,"required_sum_goodput_vs_n.png"))

def plot_jain(summary, outdir):
    agg = (summary.groupby(["transport","nSta"], as_index=False)
                 .agg(jain=("jain","mean")))
    plt.figure()
    for tr in sorted(agg["transport"].unique()):
        d = agg[agg["transport"]==tr].sort_values("nSta")
        plt.plot(d["nSta"], d["jain"], marker="o", label=tr.upper())
    plt.ylim(0,1.02)
    plt.xlabel("Number of STAs (N)")
    plt.ylabel("Jain fairness index")
    plt.title("Jain Fairness vs N")
    plt.legend()
    savefig(os.path.join(outdir,"required_jain_vs_n.png"))

def plot_persta(persta, outdir):
    latest = persta.groupby(["transport","nSta"], as_index=False)["run"].max()
    for _, row in latest.iterrows():
        tr = row["transport"]; n = int(row["nSta"]); r = int(row["run"])
        d = persta[(persta["transport"]==tr) & (persta["nSta"]==n) & (persta["run"]==r)].copy()

        # تضمین اینکه staId های 0..n-1 همه باشند (اگر نبود با صفر پر می‌کنیم)
        sta_ids = list(range(n))
        d = (d.set_index("staId")
               .reindex(sta_ids, fill_value=0)
               .reset_index())

        plt.figure()
        plt.bar(d["staId"], d["goodputbps"]/1e6)
        plt.xticks(sta_ids)              # ✅ مشکل -0.25 حل می‌شود
        plt.xlim(-0.5, n-0.5)
        plt.xlabel("STA id")
        plt.ylabel("Goodput (Mbps)")
        plt.title(f"Per-STA Goodput ({tr.upper()}), N={n}, run={r}")
        savefig(os.path.join(outdir,f"required_persta_{tr}_n{n}_run{r}.png"))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default=os.path.expanduser("~/wifi-ter-sim/results/p5"))
    ap.add_argument("--summary", default=None)
    args = ap.parse_args()

    raw_dir = os.path.join(args.base,"raw")
    outdir  = os.path.join(args.base,"plots")
    ensure_dir(outdir)

    summary_path = args.summary or os.path.join(raw_dir,"p5_summary.csv")
    summary = load_summary(summary_path)
    persta  = load_persta(raw_dir)

    plot_sum_goodput(summary, outdir)
    plot_jain(summary, outdir)
    plot_persta(persta, outdir)

    print("\n[OK] plots saved in:", outdir)
    for fp in sorted(glob.glob(os.path.join(outdir,"required_*.png"))):
        print(" -", fp)

if __name__ == "__main__":
    main()
