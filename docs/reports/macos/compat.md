# Site compatibility: `slim` against `baseline`

- Sites: 100 from `assets/WEBSITE-TOP-100.json`
- **Pass: 89, fail: 0, blocked: 11**
- Blocked means the reference build did not get the real site from the test machine either
  (unreachable, or a bot challenge or error page), so the comparison says nothing about this profile.
- Columns show this profile / the reference. Rules are in `docs/COMPAT.md`.

## Blocked

| # | Site | Why |
|---:|---|---|
| 22 | microsoft.com | reference got a challenge or error page (4 elements, title 'Error Page') |
| 25 | claude.ai | reference got a challenge or error page (47 elements, title 'Just a moment...') |
| 26 | live.com | reference got a challenge or error page (45 elements, title 'Microsoft Outlook Personal Email and Cal') |
| 42 | ebay.com | reference got a challenge or error page (24 elements, title 'Error Page / eBay') |
| 43 | vk.ru | reference got a challenge or error page (48 elements, title 'Проверяем, что вы не робот') |
| 48 | indeed.com | reference got a challenge or error page (76 elements, title 'Just a moment...') |
| 56 | ecosia.org | reference got a challenge or error page (33 elements, title 'Just a moment...') |
| 68 | quora.com | reference got a challenge or error page (47 elements, title 'Just a moment...') |
| 76 | vkvideo.ru | reference got a challenge or error page (48 elements, title 'Проверяем, что вы не робот') |
| 86 | grok.com | reference got a challenge or error page (47 elements, title 'Just a moment...') |
| 93 | fandom.com | reference got a challenge or error page (47 elements, title 'Just a moment...') |

## All sites

| # | Site | Result | Elements | Text | Painted | Script errors |
|---:|---|---|---:|---:|---:|---:|
| 1 | google.com | PASS | 98 / 98 | 97 / 97 | 2% / 2% | 0 / 0 |
| 2 | youtube.com | PASS | 2029 / 2060 | 707 / 707 | 2% / 2% | 0 / 0 |
| 3 | facebook.com | PASS | 380 / 382 | 432 / 432 | 15% / 15% | 0 / 0 |
| 4 | chatgpt.com | PASS | 723 / 723 | 1315 / 1315 | 7% / 7% | 0 / 0 |
| 5 | instagram.com | PASS | 491 / 499 | 758 / 758 | 58% / 58% | 0 / 0 |
| 6 | reddit.com | PASS | 2549 / 2550 | 10542 / 10563 | 53% / 53% | 0 / 0 |
| 7 | wikipedia.org | PASS | 1196 / 1196 | 8179 / 8179 | 8% / 8% | 0 / 0 |
| 8 | x.com | PASS | 434 / 434 | 506 / 506 | 13% / 13% | 0 / 0 |
| 9 | whatsapp.com | PASS | 1358 / 1358 | 3721 / 3721 | 80% / 80% | 0 / 0 |
| 10 | tiktok.com | PASS | 659 / 634 | 33218 / 33218 | 3% / 19% | 4 / 4 |
| 11 | amazon.com | PASS | 1821 / 1816 | 5573 / 5573 | 78% / 78% | 0 / 0 |
| 12 | yahoo.com | PASS | 1739 / 1735 | 13022 / 13181 | 57% / 57% | 0 / 0 |
| 13 | duckduckgo.com | PASS | 1712 / 1712 | 14381 / 14381 | 12% / 12% | 0 / 0 |
| 14 | bing.com | PASS | 437 / 451 | 746 / 791 | 91% / 91% | 1 / 1 |
| 15 | yahoo.co.jp | PASS | 2309 / 2446 | 5409 / 5430 | 30% / 31% | 1 / 1 |
| 16 | linkedin.com | PASS | 821 / 676 | 4838 / 2347 | 26% / 44% | 0 / 0 |
| 17 | weather.com | PASS | 1829 / 1830 | 4784 / 4784 | 52% / 52% | 1 / 1 |
| 18 | msn.com | PASS | 3045 / 3038 | 3154 / 3176 | 62% / 61% | 0 / 0 |
| 19 | netflix.com | PASS | 916 / 916 | 8082 / 8078 | 52% / 52% | 0 / 0 |
| 20 | pinterest.com | PASS | 1330 / 1325 | 2082 / 2156 | 62% / 23% | 0 / 0 |
| 21 | twitch.tv | PASS | 1629 / 1626 | 2132 / 2141 | 74% / 75% | 1 / 1 |
| 22 | microsoft.com | BLOCKED | 3 / 4 | 0 / 78 | 0% / 0% | 0 / 0 |
| 23 | github.com | PASS | 1816 / 1816 | 7734 / 7734 | 63% / 63% | 0 / 0 |
| 24 | temu.com | PASS | 88 / 88 | 513 / 513 | 33% / 33% | 0 / 0 |
| 25 | claude.ai | BLOCKED | 47 / 47 | 321 / 321 | 3% / 3% | 0 / 0 |
| 26 | live.com | BLOCKED | 20 / 45 | 247 / 61 | 5% / 0% | 0 / 0 |
| 27 | yandex.ru | PASS | 1335 / 1304 | 4943 / 4896 | 34% / 34% | 0 / 0 |
| 28 | discord.com | PASS | 1205 / 1205 | 3948 / 3948 | 93% / 93% | 1 / 1 |
| 29 | naver.com | PASS | 1433 / 1431 | 3236 / 3212 | 18% / 19% | 0 / 0 |
| 30 | spotify.com | PASS | 1456 / 1099 | 8791 / 2779 | 68% / 66% | 1 / 1 |
| 31 | canva.com | PASS | 2514 / 2514 | 8486 / 8486 | 79% / 79% | 0 / 0 |
| 32 | imdb.com | PASS | 138 / 139 | 69 / 69 | 0% / 0% | 0 / 0 |
| 33 | apple.com | PASS | 1943 / 1943 | 9002 / 9002 | 36% / 36% | 0 / 0 |
| 34 | paypal.com | PASS | 76 / 79 | 69 / 69 | 1% / 1% | 0 / 0 |
| 35 | aliexpress.com | PASS | 99 / 99 | 163 / 163 | 3% / 3% | 0 / 0 |
| 36 | roblox.com | PASS | 516 / 516 | 1202 / 1202 | 60% / 60% | 0 / 0 |
| 37 | globo.com | PASS | 3511 / 3512 | 16829 / 16868 | 25% / 32% | 3 / 3 |
| 38 | telegram.org | PASS | 306 / 306 | 1879 / 1879 | 11% / 11% | 0 / 0 |
| 39 | openai.com | PASS | 1071 / 1071 | 4808 / 4808 | 33% / 33% | 0 / 0 |
| 40 | booking.com | PASS | 110 / 110 | 1629 / 1629 | 12% / 12% | 0 / 0 |
| 41 | dailymotion.com | PASS | 1442 / 1446 | 3743 / 3699 | 38% / 39% | 0 / 0 |
| 42 | ebay.com | BLOCKED | 24 / 24 | 200 / 200 | 10% / 10% | 0 / 0 |
| 43 | vk.ru | BLOCKED | 48 / 48 | 164 / 164 | 9% / 9% | 0 / 0 |
| 44 | walmart.com | PASS | 1325 / 1588 | 5497 / 5348 | 51% / 52% | 5 / 5 |
| 45 | adobe.com | PASS | 2608 / 2607 | 14481 / 14481 | 88% / 88% | 0 / 0 |
| 46 | amazon.de | PASS | 2466 / 2419 | 11713 / 11534 | 46% / 44% | 0 / 0 |
| 47 | nytimes.com | PASS | 6823 / 7039 | 29428 / 30025 | 24% / 24% | 0 / 0 |
| 48 | indeed.com | BLOCKED | 76 / 76 | 350 / 350 | 4% / 4% | 0 / 0 |
| 49 | shopify.com | PASS | 1403 / 1749 | 7904 / 6557 | 6% / 53% | 0 / 0 |
| 50 | usps.com | PASS | 966 / 966 | 6674 / 6674 | 60% / 60% | 0 / 0 |
| 51 | zoom.us | PASS | 1910 / 1911 | 15012 / 15012 | 76% / 76% | 0 / 0 |
| 52 | samsung.com | PASS | 3857 / 3851 | 10310 / 10310 | 46% / 46% | 1 / 1 |
| 53 | office.com | PASS | 1124 / 1124 | 19496 / 19506 | 37% / 37% | 0 / 0 |
| 54 | uol.com.br | PASS | 3654 / 3638 | 15146 / 15122 | 30% / 30% | 2 / 2 |
| 55 | amazon.co.jp | PASS | 2340 / 2386 | 4817 / 5026 | 69% / 69% | 0 / 0 |
| 56 | ecosia.org | BLOCKED | 33 / 33 | 229 / 229 | 2% / 2% | 0 / 0 |
| 57 | disneyplus.com | PASS | 1226 / 1216 | 5911 / 5911 | 41% / 0% | 0 / 0 |
| 58 | espn.com | PASS | 4316 / 4271 | 16434 / 16428 | 45% / 69% | 0 / 0 |
| 59 | etsy.com | PASS | 1011 / 1011 | 4423 / 4413 | 66% / 65% | 0 / 0 |
| 60 | bbc.co.uk | PASS | 3312 / 3319 | 13599 / 13577 | 2% / 2% | 1 / 1 |
| 61 | bbc.com | PASS | 2728 / 2732 | 15925 / 15955 | 30% / 30% | 0 / 0 |
| 62 | rakuten.co.jp | PASS | 4742 / 4742 | 9658 / 9632 | 58% / 55% | 0 / 0 |
| 63 | deepseek.com | PASS | 322 / 322 | 785 / 785 | 66% / 66% | 0 / 0 |
| 64 | primevideo.com | PASS | 858 / 858 | 1695 / 1695 | 38% / 38% | 0 / 0 |
| 65 | threads.com | PASS | 1860 / 1584 | 2710 / 2420 | 26% / 25% | 1 / 1 |
| 66 | accuweather.com | PASS | 665 / 663 | 1274 / 1274 | 61% / 61% | 0 / 0 |
| 67 | scribd.com | PASS | 1238 / 1238 | 4374 / 4374 | 60% / 60% | 0 / 0 |
| 68 | quora.com | BLOCKED | 47 / 47 | 329 / 329 | 3% / 3% | 0 / 0 |
| 69 | mail.ru | PASS | 395 / 392 | 1963 / 1913 | 14% / 14% | 0 / 0 |
| 70 | yandex.com | PASS | 203 / 203 | 271 / 269 | 4% / 4% | 0 / 0 |
| 71 | shein.com | PASS | 1023 / 1022 | 858 / 858 | 82% / 82% | 0 / 0 |
| 72 | amazon.in | PASS | 2260 / 2261 | 9008 / 9042 | 84% / 84% | 0 / 0 |
| 73 | amazon.co.uk | PASS | 2243 / 2239 | 9386 / 9510 | 50% / 50% | 0 / 0 |
| 74 | cnn.com | PASS | 4072 / 4072 | 15151 / 15151 | 59% / 59% | 0 / 0 |
| 75 | ozon.ru | PASS | 1385 / 1397 | 2786 / 2963 | 62% / 71% | 0 / 0 |
| 76 | vkvideo.ru | BLOCKED | 1459 / 48 | 2712 / 164 | 55% / 9% | 0 / 0 |
| 77 | marca.com | PASS | 4405 / 4393 | 22364 / 22480 | 59% / 59% | 0 / 0 |
| 78 | zillow.com | PASS | 1330 / 1330 | 7786 / 7786 | 49% / 49% | 0 / 0 |
| 79 | ikea.com | PASS | 1512 / 1512 | 6978 / 6978 | 61% / 61% | 0 / 0 |
| 80 | labs.google | PASS | 1048 / 1048 | 4156 / 4156 | 90% / 83% | 0 / 0 |
| 81 | opera.com | PASS | 776 / 776 | 3391 / 3391 | 48% / 48% | 0 / 0 |
| 82 | instructure.com | PASS | 2742 / 2728 | 20979 / 20817 | 30% / 30% | 1 / 1 |
| 83 | hbomax.com | PASS | 924 / 924 | 3738 / 3738 | 50% / 50% | 0 / 0 |
| 84 | archive.org | PASS | 1304 / 1304 | 5994 / 5994 | 43% / 43% | 0 / 0 |
| 85 | theguardian.com | PASS | 4863 / 4882 | 18338 / 18504 | 37% / 37% | 0 / 0 |
| 86 | grok.com | BLOCKED | 47 / 47 | 319 / 319 | 3% / 3% | 0 / 0 |
| 87 | gemini.google.com | PASS | 516 / 480 | 435 / 366 | 64% / 64% | 1 / 1 |
| 88 | cloud.microsoft | PASS | 1144 / 1124 | 19904 / 19506 | 40% / 37% | 0 / 0 |
| 89 | baidu.com | PASS | 537 / 537 | 260987 / 261109 | 5% / 5% | 0 / 0 |
| 90 | bilibili.com | PASS | 65 / 65 | 173 / 173 | 3% / 3% | 0 / 0 |
| 91 | dzen.ru | PASS | 1334 / 231 | 5193 / 166 | 34% / 5% | 0 / 1 |
| 92 | news.yahoo.co.jp | PASS | 2513 / 2522 | 6771 / 6811 | 17% / 17% | 0 / 0 |
| 93 | fandom.com | BLOCKED | 47 / 47 | 331 / 331 | 4% / 4% | 0 / 0 |
| 94 | brave.com | PASS | 2894 / 2894 | 10743 / 10743 | 40% / 40% | 0 / 0 |
| 95 | t.me | PASS | 306 / 306 | 1879 / 1879 | 11% / 11% | 0 / 0 |
| 96 | realtor.com | PASS | 1101 / 1551 | 4598 / 5278 | 90% / 90% | 0 / 0 |
| 97 | fifa.com | PASS | 2459 / 1574 | 12276 / 8303 | 68% / 68% | 0 / 0 |
| 98 | mashable.com | PASS | 1375 / 1402 | 12286 / 12286 | 77% / 77% | 1 / 1 |
| 99 | flipkart.com | PASS | 2085 / 2085 | 21554 / 21548 | 68% / 67% | 0 / 0 |
| 100 | xsmn.mobi | PASS | 2588 / 2588 | 14522 / 14522 | 24% / 24% | 0 / 0 |
