# Cell health log
Dated notes from reviewing logged rides for pack imbalance/degradation — a running record to compare against on future rides, not something the firmware generates. Kept out of the README/git history since it's personal pack data, not project documentation.

## 2026-09-18
Data reviewed: ride logged 2026-09-14 19:19 (`data.csv` export, 4189 rows, 26-cell pack).

- **cell26 looks weak.** Avg 3.766V vs 3.89–3.92V for every other cell; sags to 3.275V min vs 3.70–3.77V for the rest under the same high-current rows (e.g. at packI≈228A, cell26 read 3.328V while every other cell was 3.70–3.86V). Spread across the ride was 0.575V vs 0.16–0.21V for healthy cells. Lower resting voltage *and* far larger sag under load than the rest — consistent with higher internal resistance/reduced capacity rather than normal balancing variation. Worth checking its connections/busbar and watching whether the gap widens on future rides.
- cell1, cell14, cell24 had slightly wider spreads than the pack norm (~0.21V vs ~0.17V) — not alarming yet, just worth re-checking next time.
- All other cells tracked tightly together across the whole current range, no other outliers.
