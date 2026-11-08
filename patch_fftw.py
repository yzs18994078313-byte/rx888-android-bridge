"""Enable FFTW 3.3.10's NEON source on AArch64, without ARM32 instruction probing."""
from pathlib import Path
root=Path(__file__).resolve().parent/'src/fftw-3.3.10'
p=root/'kernel/ifftw.h'
s=p.read_text()
s=s.replace('#if defined(HAVE_SSE) || defined(HAVE_SSE2) ||', '#if defined(HAVE_NEON) || defined(HAVE_SSE) || defined(HAVE_SSE2) ||')
if p.read_text()!=s: p.write_text(s)
p=root/'simd-support/neon.c'
s=p.read_text().replace('#if defined(unix) || defined(linux)', '#if (defined(unix) || defined(linux)) && !defined(__aarch64__)')
if p.read_text()!=s: p.write_text(s)
