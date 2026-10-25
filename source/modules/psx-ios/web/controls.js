'use strict';
globalThis.WeatherUnits = (() => {
  // Keep API/PSX values in hPa and integral Celsius. Only the editor changes units.
  // PSX's packed pressure uses 0.33863887 hPa per hundredth inHg.
  const hpaPerInHg = 33.863887;
  const temperature = key => key === 'surface_temperature' || key === 'inversion_temperature';
  const display = (key, value, units) => units === 'imperial'
    ? key === 'qnh' ? (value / hpaPerInHg).toFixed(2)
      : temperature(key) ? String(Number((value * 1.8 + 32).toFixed(1))) : String(value)
    : key === 'qnh' ? String(Math.round(value)) : String(value);
  const native = (key, value, units) => units === 'imperial'
    ? key === 'qnh' ? value * hpaPerInHg
      : temperature(key) ? Math.round((value - 32) / 1.8) : value
    : value;
  return {display, native, temperature};
})();
// Small shared input arithmetic. Rendering and transport remain in their existing owners.
globalThis.VisibilityScale = (() => {
  const max = 40000, span = Math.log1p(max / 1000), ticks = 10000;
  const nativeVisibilityMetres = 9999;

  // Visibility is in metres; the logarithmic slider gives short ranges more space.
  const step = value => value < 1000 ? 50 : value < 5000 ? 100 : 500;
  const position = value => Math.log1p(Math.max(0, Math.min(max, value)) / 1000) / span * ticks;
  const snap = value => {
    value = Math.max(0, Math.min(max, value));
    const rounded = Math.round(value / step(value)) * step(value);
    // Keep PSX's native 9,999 m limit selectable beside the ordinary increments.
    return Math.abs(value - nativeVisibilityMetres) < Math.abs(value - rounded)
      ? nativeVisibilityMetres : rounded;
  };
  const metres = position => snap(Math.expm1(Math.max(0, Math.min(ticks, position)) / ticks * span) * 1000);

  return {max, ticks, step, position, metres, snap};
})();
globalThis.WindControls = (() => {
  // Directions are degrees, wrapped through north after snapping to 10 degrees.
  const wrap = degrees => ((degrees % 360) + 360) % 360;
  const direction = degrees => wrap(Math.round(degrees / 10) * 10);

  function strength(speed, peak, mode, value, limit = 50) {
    value = Math.max(0, Math.min(limit, Math.round(value)));
    if (mode === 'speed') {
      // Speeds are knots. Retain the gust increment while limiting the peak to 75 kt.
      const gustIncrement = Math.max(0, peak - speed);
      speed = Math.min(value, 75 - gustIncrement);
      peak = speed + gustIncrement;
    } else {
      peak = value;
      speed = Math.min(speed, peak);
    }
    return {speed, gust: peak};
  }

  return {direction, strength};
})();
