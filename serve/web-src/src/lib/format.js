// Number and time formatting shared by the views. "–" stands for a value the server did not report.
export const fmt = (n, d = 0) =>
  n == null || Number.isNaN(n) ? "–" : Number(n).toLocaleString(undefined, {maximumFractionDigits: d, minimumFractionDigits: d});
export const kfmt = (n) => (n == null ? "–" : n >= 1000 ? `${fmt(n / 1000, n >= 10000 ? 0 : 1)}k` : fmt(n));
// a context size: 32768 -> "32K" (powers of two), else like kfmt
export const ctxfmt = (n) => (n && n % 1024 === 0 ? `${fmt(n / 1024)}K` : kfmt(n));
export const gb = (b, d = 1) => (b == null ? "–" : fmt(b / 1073741824, d));   // memory: binary GB, as Windows shows it

// clock times in 24 hours, in the browser's language
export const clock = (secs, seconds = true) =>
  new Date(secs * 1000).toLocaleTimeString([], {hour: "2-digit", minute: "2-digit", ...(seconds ? {second: "2-digit"} : {}), hourCycle: "h23"});
export const dateTime = (secs) =>
  new Date(secs * 1000).toLocaleString([], {month: "short", day: "numeric", hour: "2-digit", minute: "2-digit", hourCycle: "h23"});
