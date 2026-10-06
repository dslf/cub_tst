# validate 54-sticker state lines: centers fixed, 9 stickers per colour
/^[UDLRFB]{54}$/ {
  n++
  for (f = 0; f < 6; f++)
    if (substr($0, f*9+5, 1) != substr("UDLRFB", f+1, 1)) { print "CENTER BAD line " NR; bad = 1 }
  delete cnt
  for (i = 1; i <= 54; i++) cnt[substr($0, i, 1)]++
  for (k in cnt) if (cnt[k] != 9) { print "COLOR BAD line " NR; bad = 1; next }
}
END { printf "%d states checked -> %s\n", n, (bad ? "FAIL" : "PASS") }
