"""Token ids whose text contains a script the old llama.cpp no-hanja.gbnf banned (CJK ideographs, kana, Cyrillic,
Turkish-specific letters), plus byte-fragment tokens whose trailing partial UTF-8 sequence can only become one.
Also bans Thai (U+0E00..U+0E7F) and every vocabulary token containing em dash (U+2014).
Hangul (syllables, jamo) is never banned.  Writes int32 ids."""
import json, struct, sys

RANGES = [(0xC7, 0xC7), (0xD6, 0xD6), (0xDC, 0xDC), (0xE7, 0xE7), (0xF6, 0xF6), (0xFC, 0xFC), (0x11E, 0x11F),
          (0x130, 0x131), (0x15E, 0x15F), (0x400, 0x52F), (0xE00, 0xE7F), (0x1C80, 0x1C8F), (0x2DE0, 0x2DFF), (0x3040, 0x30FF),
          (0x31F0, 0x31FF), (0x3400, 0x4DBF), (0xA640, 0xA69F), (0xF900, 0xFAFF), (0xFE2E, 0xFE2F), (0xFF65, 0xFF9F),
          (0x4E00, 0x9FFF), (0x1AFF0, 0x1AFFF), (0x1B000, 0x1B12F), (0x20000, 0x2A6DF), (0x2A700, 0x2EE5F),
          (0x2F800, 0x2FA1F), (0x30000, 0x323AF)]

def banned_cp(c):
    return c == 0x2014 or any(a <= c <= b for a, b in RANGES)

def byte_decoder():
    bs = list(range(ord("!"), ord("~") + 1)) + list(range(ord("¡"), ord("¬") + 1)) + list(range(ord("®"), ord("ÿ") + 1))
    cs, n = bs[:], 0
    for b in range(256):
        if b not in bs:
            bs.append(b); cs.append(256 + n); n += 1
    return {chr(c): b for b, c in zip(bs, cs)}

def partial_banned(tail: bytes) -> bool:
    """A trailing incomplete UTF-8 sequence that can only complete into a banned code point."""
    lead = tail[0]
    if lead == 0xE0 and len(tail) >= 2:            # Thai U+0E00..U+0E7F: E0 B8/B9 xx
        return tail[1] in (0xB8, 0xB9)
    if 0xE4 <= lead <= 0xE9:                       # U+4000..U+9FFF: CJK Ext A tail / unified ideographs (+ Yi)
        return True
    if lead == 0xE3 and len(tail) >= 2:            # U+3000..U+3FFF: kana E3 81-83, Ext A E3 90-BF
        return 0x81 <= tail[1] <= 0x83 or tail[1] >= 0x90
    if lead == 0xD0 or lead == 0xD1 or lead == 0xD2 or lead == 0xD3:   # Cyrillic U+0400-04FF
        return True
    if lead == 0xF0 and len(tail) >= 2:            # U+20000..U+3FFFF: Ext B..
        return 0xA0 <= tail[1] <= 0xB2
    return False

def main(vocab_path, out_path):
    vocab = json.load(open(vocab_path, encoding="utf-8"))
    dec = byte_decoder()
    ban = []
    for tok, i in vocab.items():
        if tok.startswith("<|") and tok.endswith("|>"):
            continue
        try:
            raw = bytes(dec[ch] for ch in tok)
        except KeyError:
            continue
        text = raw.decode("utf-8", errors="ignore")
        if any(banned_cp(ord(ch)) for ch in text):
            ban.append(i); continue
        # trailing partial sequence
        k = len(raw)
        j = k - 1
        while j >= 0 and 0x80 <= raw[j] <= 0xBF and k - j < 4:
            j -= 1
        if j >= 0 and raw[j] >= 0xC0:
            need = 2 if raw[j] < 0xE0 else 3 if raw[j] < 0xF0 else 4
            if k - j < need and partial_banned(raw[j:]):
                ban.append(i)
    ban.sort()
    open(out_path, "wb").write(struct.pack(f"<{len(ban)}i", *ban))
    print(f"{len(ban)} banned of {len(vocab)}")

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
