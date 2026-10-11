# Site compatibility: `slim` against `baseline`

- Sites: 100 from `assets/WEBSITE-TOP-100.json`
- **Pass: 84, fail: 0, blocked: 16**
- Blocked means the reference build did not get the real site from the test machine either
  (unreachable, or a bot challenge or error page), so the comparison says nothing about this profile.
- Columns show this profile / the reference. Rules are in `docs/COMPAT.md`.

## Blocked

| # | Site | Why |
|---:|---|---|
| 25 | claude.ai | reference got a challenge or error page (43 elements, title 'Just a moment...') |
| 26 | live.com | reference got a challenge or error page (42 elements, title 'Microsoft Outlook Personal Email and Cal') |
| 39 | openai.com | reference got a challenge or error page (35 elements, title 'Just a moment...') |
| 42 | ebay.com | reference got a challenge or error page (24 elements, title 'Error Page / eBay') |
| 43 | vk.ru | reference got a challenge or error page (48 elements, title 'Проверяем, что вы не робот') |
| 47 | nytimes.com | reference got a challenge or error page (9 elements, title 'nytimes.com') |
| 48 | indeed.com | reference got a challenge or error page (76 elements, title 'Just a moment...') |
| 56 | ecosia.org | reference got a challenge or error page (29 elements, title 'Just a moment...') |
| 58 | espn.com | reference: load failed: page stopped responding |
| 68 | quora.com | reference got a challenge or error page (47 elements, title 'Just a moment...') |
| 75 | ozon.ru | reference got a challenge or error page (22 elements, title 'Похоже, нет соединения') |
| 76 | vkvideo.ru | reference got a challenge or error page (48 elements, title 'Проверяем, что вы не робот') |
| 90 | bilibili.com | reference got a challenge or error page (3 elements, title '') |
| 91 | dzen.ru | reference got a challenge or error page (3 elements, title '') |
| 93 | fandom.com | reference got a challenge or error page (43 elements, title 'Just a moment...') |
| 96 | realtor.com | reference got a challenge or error page (45 elements, title 'realtor.com') |

## All sites

| # | Site | Result | Elements | Text | Painted | Script errors |
|---:|---|---|---:|---:|---:|---:|
| 1 | google.com | PASS | 521 / 513 | 642 / 508 | 2% / 2% | 0 / 0 |
| 2 | youtube.com | PASS | 2062 / 2029 | 652 / 515 | 2% / 2% | 0 / 0 |
| 3 | facebook.com | PASS | 378 / 376 | 436 / 436 | 15% / 15% | 0 / 0 |
| 4 | chatgpt.com | PASS | 879 / 874 | 1584 / 1584 | 4% / 4% | 0 / 0 |
| 5 | instagram.com | PASS | 504 / 496 | 758 / 758 | 15% / 15% | 0 / 0 |
| 6 | reddit.com | PASS | 2612 / 2602 | 11351 / 11406 | 36% / 35% | 2 / 2 |
| 7 | wikipedia.org | PASS | 1196 / 1196 | 8188 / 8188 | 35% / 35% | 0 / 0 |
| 8 | x.com | PASS | 424 / 424 | 474 / 474 | 11% / 11% | 0 / 0 |
| 9 | whatsapp.com | PASS | 1358 / 1358 | 3678 / 3721 | 79% / 79% | 0 / 0 |
| 10 | tiktok.com | PASS | 733 / 714 | 489 / 380 | 37% / 37% | 0 / 0 |
| 11 | amazon.com | PASS | 1724 / 1843 | 7455 / 8129 | 65% / 68% | 0 / 5 |
| 12 | yahoo.com | PASS | 2381 / 2520 | 7423 / 9178 | 42% / 74% | 0 / 0 |
| 13 | duckduckgo.com | PASS | 1751 / 1751 | 14600 / 14600 | 6% / 6% | 0 / 0 |
| 14 | bing.com | PASS | 625 / 632 | 1566 / 1560 | 84% / 89% | 1 / 1 |
| 15 | yahoo.co.jp | PASS | 2370 / 2356 | 5595 / 5619 | 33% / 26% | 1 / 1 |
| 16 | linkedin.com | PASS | 794 / 819 | 4857 / 4865 | 25% / 25% | 0 / 0 |
| 17 | weather.com | PASS | 2156 / 2140 | 5267 / 5254 | 61% / 62% | 2 / 2 |
| 18 | msn.com | PASS | 2676 / 2654 | 3278 / 3259 | 41% / 25% | 1 / 1 |
| 19 | netflix.com | PASS | 1084 / 1084 | 8178 / 8204 | 58% / 58% | 0 / 0 |
| 20 | pinterest.com | PASS | 1291 / 1304 | 2086 / 2086 | 29% / 29% | 0 / 0 |
| 21 | twitch.tv | PASS | 1639 / 1616 | 2338 / 2277 | 61% / 61% | 0 / 1 |
| 22 | microsoft.com | PASS | 4401 / 4409 | 7324 / 7376 | 55% / 21% | 4 / 4 |
| 23 | github.com | PASS | 1812 / 1812 | 7734 / 7734 | 67% / 67% | 0 / 0 |
| 24 | temu.com | PASS | 6786 / 6622 | 23934 / 22702 | 53% / 53% | 1 / 1 |
| 25 | claude.ai | BLOCKED | 43 / 43 | 321 / 321 | 1% / 1% | 0 / 0 |
| 26 | live.com | BLOCKED | 129 / 42 | 358 / 61 | 0% / 0% | 0 / 0 |
| 27 | yandex.ru | PASS | 1154 / 1160 | 5088 / 4228 | 24% / 24% | 0 / 0 |
| 28 | discord.com | PASS | 1197 / 1197 | 3918 / 3918 | 93% / 93% | 1 / 1 |
| 29 | naver.com | PASS | 1449 / 953 | 3112 / 2552 | 28% / 19% | 0 / 0 |
| 30 | spotify.com | PASS | 2931 / 2297 | 10142 / 5301 | 45% / 47% | 1 / 1 |
| 31 | canva.com | PASS | 2463 / 2463 | 8205 / 8205 | 94% / 94% | 0 / 0 |
| 32 | imdb.com | PASS | 1744 / 1744 | 3274 / 3309 | 44% / 44% | 0 / 0 |
| 33 | apple.com | PASS | 1944 / 1944 | 8979 / 8979 | 36% / 36% | 0 / 0 |
| 34 | paypal.com | PASS | 4116 / 4154 | 10620 / 10620 | 35% / 35% | 0 / 0 |
| 35 | aliexpress.com | PASS | 4083 / 4064 | 29850 / 29721 | 61% / 61% | 2 / 2 |
| 36 | roblox.com | PASS | 574 / 351 | 989 / 522 | 85% / 88% | 0 / 0 |
| 37 | globo.com | PASS | 3412 / 3410 | 16125 / 16049 | 23% / 23% | 2 / 2 |
| 38 | telegram.org | PASS | 306 / 306 | 1879 / 1879 | 12% / 12% | 0 / 0 |
| 39 | openai.com | BLOCKED | 1069 / 35 | 4808 / 74 | 12% / 0% | 0 / 0 |
| 40 | booking.com | PASS | 2246 / 151 | 5635 / 84 | 67% / 0% | 0 / 0 |
| 41 | dailymotion.com | PASS | 1436 / 1433 | 3678 / 3711 | 35% / 12% | 0 / 0 |
| 42 | ebay.com | BLOCKED | 24 / 24 | 200 / 200 | 10% / 10% | 0 / 0 |
| 43 | vk.ru | BLOCKED | 280 / 48 | 245 / 164 | 80% / 9% | 1 / 0 |
| 44 | walmart.com | PASS | 1124 / 1124 | 1659 / 1661 | 78% / 77% | 16 / 16 |
| 45 | adobe.com | PASS | 1383 / 1136 | 9558 / 7789 | 92% / 92% | 0 / 0 |
| 46 | amazon.de | PASS | 1230 / 1399 | 4847 / 7592 | 68% / 58% | 0 / 2 |
| 47 | nytimes.com | BLOCKED | 7951 / 9 | 31935 / 11 | 56% / 1% | 1 / 0 |
| 48 | indeed.com | BLOCKED | 76 / 76 | 350 / 350 | 2% / 2% | 0 / 0 |
| 49 | shopify.com | PASS | 1777 / 1777 | 6854 / 6854 | 58% / 58% | 0 / 0 |
| 50 | usps.com | PASS | 967 / 967 | 6674 / 6674 | 57% / 57% | 0 / 0 |
| 51 | zoom.us | PASS | 59 / 64 | 30 / 30 | 0% / 0% | 0 / 0 |
| 52 | samsung.com | PASS | 4064 / 4064 | 10992 / 10992 | 55% / 55% | 1 / 1 |
| 53 | office.com | PASS | 1124 / 1124 | 19434 / 19480 | 36% / 36% | 0 / 0 |
| 54 | uol.com.br | PASS | 3672 / 3743 | 15017 / 15065 | 56% / 57% | 2 / 2 |
| 55 | amazon.co.jp | PASS | 2380 / 2361 | 5113 / 5109 | 65% / 66% | 0 / 0 |
| 56 | ecosia.org | BLOCKED | 393 / 29 | 2461 / 229 | 36% / 1% | 0 / 0 |
| 57 | disneyplus.com | PASS | 3877 / 3877 | 32124 / 32124 | 47% / 61% | 0 / 0 |
| 58 | espn.com | BLOCKED | - / - | - / - | - / - | - / - |
| 59 | etsy.com | PASS | 968 / 968 | 4905 / 4911 | 62% / 66% | 1 / 1 |
| 60 | bbc.co.uk | PASS | 2814 / 2820 | 16326 / 16286 | 29% / 30% | 0 / 0 |
| 61 | bbc.com | PASS | 2896 / 2883 | 16352 / 16312 | 29% / 30% | 0 / 0 |
| 62 | rakuten.co.jp | PASS | 4702 / 4650 | 9633 / 9517 | 34% / 34% | 2 / 2 |
| 63 | deepseek.com | PASS | 318 / 338 | 759 / 1034 | 73% / 76% | 0 / 0 |
| 64 | primevideo.com | PASS | 2038 / 2059 | 3445 / 3494 | 70% / 38% | 0 / 0 |
| 65 | threads.com | PASS | 1011 / 951 | 1640 / 1181 | 28% / 27% | 0 / 0 |
| 66 | accuweather.com | PASS | 1065 / 1065 | 6098 / 6097 | 78% / 79% | 0 / 0 |
| 67 | scribd.com | PASS | 1246 / 1246 | 4413 / 4413 | 48% / 48% | 0 / 0 |
| 68 | quora.com | BLOCKED | 43 / 47 | 329 / 329 | 1% / 2% | 0 / 0 |
| 69 | mail.ru | PASS | 710 / 699 | 1969 / 1894 | 20% / 20% | 0 / 0 |
| 70 | yandex.com | PASS | 204 / 230 | 263 / 263 | 1% / 1% | 0 / 0 |
| 71 | shein.com | PASS | 1782 / 1780 | 5748 / 5754 | 48% / 48% | 0 / 0 |
| 72 | amazon.in | PASS | 1339 / 1322 | 4499 / 4093 | 65% / 65% | 0 / 1 |
| 73 | amazon.co.uk | PASS | 1638 / 1675 | 8314 / 8394 | 50% / 51% | 0 / 2 |
| 74 | cnn.com | PASS | 5296 / 5332 | 20672 / 20974 | 76% / 63% | 1 / 1 |
| 75 | ozon.ru | BLOCKED | 22 / 22 | 204 / 204 | 2% / 2% | 0 / 0 |
| 76 | vkvideo.ru | BLOCKED | 1475 / 48 | 2554 / 164 | 52% / 9% | 0 / 0 |
| 77 | marca.com | PASS | 4448 / 4627 | 22804 / 23552 | 54% / 54% | 2 / 2 |
| 78 | zillow.com | PASS | 1639 / 1639 | 9405 / 9423 | 62% / 62% | 0 / 0 |
| 79 | ikea.com | PASS | 1511 / 1511 | 6953 / 6953 | 60% / 60% | 0 / 0 |
| 80 | labs.google | PASS | 978 / 978 | 4156 / 4156 | 94% / 94% | 0 / 0 |
| 81 | opera.com | PASS | 776 / 776 | 3391 / 3391 | 40% / 40% | 0 / 0 |
| 82 | instructure.com | PASS | 2739 / 2735 | 20866 / 20812 | 30% / 30% | 1 / 1 |
| 83 | hbomax.com | PASS | 1660 / 1680 | 216148 / 216148 | 36% / 36% | 0 / 0 |
| 84 | archive.org | PASS | 1304 / 1304 | 6061 / 6061 | 43% / 43% | 0 / 0 |
| 85 | theguardian.com | PASS | 5329 / 5278 | 20333 / 19779 | 46% / 43% | 1 / 1 |
| 86 | grok.com | PASS | 571 / 570 | 3259 / 3259 | 3% / 3% | 0 / 0 |
| 87 | gemini.google.com | PASS | 488 / 463 | 453 / 376 | 40% / 40% | 1 / 1 |
| 88 | cloud.microsoft | PASS | 1124 / 1124 | 19434 / 19433 | 36% / 36% | 0 / 0 |
| 89 | baidu.com | PASS | 537 / 537 | 262554 / 262473 | 5% / 5% | 0 / 0 |
| 90 | bilibili.com | BLOCKED | 3 / 3 | 23 / 23 | 0% / 0% | 0 / 0 |
| 91 | dzen.ru | BLOCKED | 3 / 3 | 0 / 0 | 0% / 0% | 0 / 0 |
| 92 | news.yahoo.co.jp | PASS | 2454 / 2448 | 7220 / 7327 | 30% / 30% | 0 / 0 |
| 93 | fandom.com | BLOCKED | 47 / 43 | 331 / 331 | 2% / 1% | 0 / 0 |
| 94 | brave.com | PASS | 2894 / 2894 | 10743 / 10743 | 40% / 40% | 0 / 0 |
| 95 | t.me | PASS | 306 / 306 | 1879 / 1879 | 12% / 12% | 0 / 0 |
| 96 | realtor.com | BLOCKED | 45 / 45 | 258 / 258 | 2% / 0% | 1 / 1 |
| 97 | fifa.com | PASS | 1019 / 880 | 7005 / 3890 | 60% / 41% | 0 / 0 |
| 98 | mashable.com | PASS | 1356 / 1361 | 12240 / 12302 | 76% / 76% | 1 / 1 |
| 99 | flipkart.com | PASS | 2252 / 2244 | 21672 / 21562 | 68% / 64% | 0 / 0 |
| 100 | xsmn.mobi | PASS | 2602 / 2605 | 14512 / 14521 | 24% / 23% | 0 / 0 |
