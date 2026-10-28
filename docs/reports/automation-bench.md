# Playwright's commands: native, zebkit's endpoint and over CDP

`scripts/automation-bench.sh`, 50 runs of each (20 of `goto` and `screenshot`) after three to warm up,
headless, tests/bench/automation/index.html served locally. Playwright is playwright-core 1.64.1 in Node.js.
Medians in milliseconds; p90 in parentheses.

| Operation | zebkit native | zebkit endpoint (Playwright protocol) (155.0.8059.84) | zebkit over CDP (155.0.8059.84) | Chrome over CDP (156.0.8078.4) | native vs over CDP |
|---|---|---|---|---|---|
| goto | 15.62 (17.04) | 16.72 (18.77) | 14.74 (21.22) | 17.08 (20.59) | slower |
| evaluate 1+1 | 0.12 (0.15) | 0.59 (1.04) | 0.60 (0.82) | 0.45 (0.62) | 5.0x faster |
| locator.textContent | 0.22 (0.30) | 0.65 (0.73) | 1.10 (1.18) | 0.90 (1.09) | 5.0x faster |
| locator.click | 4.91 (5.15) | 3.46 (5.09) | 33.39 (33.68) | 33.39 (33.76) | 6.8x faster |
| locator.fill | 0.66 (2.51) | 2.42 (2.64) | 3.22 (5.16) | 2.74 (5.00) | 4.9x faster |
| locator.press Enter | 0.40 (0.56) | 0.82 (0.95) | 3.18 (3.37) | 2.34 (2.97) | 8.0x faster |
| locator.isVisible | 0.20 (0.23) | 0.69 (0.77) | 0.74 (0.80) | 0.66 (0.80) | 3.7x faster |
| locator.count (200) | 0.24 (0.28) | 0.77 (1.03) | 0.78 (0.89) | 0.76 (0.94) | 3.3x faster |
| allTextContents (200) | 0.26 (0.30) | 2.05 (2.26) | 2.21 (2.61) | 1.81 (2.68) | 8.5x faster |
| getByRole click | 3.19 (5.23) | 3.46 (4.59) | 33.32 (33.81) | 33.35 (33.65) | 10.4x faster |
| screenshot | 29.91 (31.80) | 44.15 (44.65) | 66.68 (67.85) | 33.39 (35.25) | 2.2x faster |
