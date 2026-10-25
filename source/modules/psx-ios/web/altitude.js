'use strict';
globalThis.AltitudeScale = (() => {
  // Heights and editing increments are feet; display coordinates belong to the caller.
  const step = height => height < 5000 ? 200 : height < 10000 ? 500 : 1000;
  const snap = (height, min, max) => Math.max(min, Math.min(max, Math.round(height / step(height)) * step(height)));
  // Descending from a boundary uses the smaller increment immediately below it.
  const next = (height, direction) => height + direction * step(direction < 0 ? height - 1 : height);

  function create(max, top, bottom) {
    const logarithmicSpan = Math.log1p(max / 1000), displaySpan = bottom - top;
    // A logarithmic projection leaves more editing space near the runway/cloud base.
    return {
      y: height => bottom - Math.log1p(Math.max(0, height) / 1000) / logarithmicSpan * displaySpan,
      height: y => Math.expm1((bottom - Math.max(top, Math.min(bottom, y))) / displaySpan * logarithmicSpan) * 1000
    };
  }

  return {step, snap, next, create};
})();
