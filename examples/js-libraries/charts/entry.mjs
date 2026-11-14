// A library of Apache ECharts (https://echarts.apache.org): a chart's option
// to an SVG, drawn on the server side (ssr: true), with no canvas and no DOM.
// Bundled into a folder by build.sh (tools/js-bundle.mjs).
import * as echarts from 'echarts';

/**
 * The chart as an SVG document.
 * @param {{width?: number, height?: number, option: object}} spec the size (600 by 400) and ECharts' option
 * @returns {string}
 */
export function renderSvg({width = 600, height = 400, option}) {
  const chart = echarts.init(null, null, {renderer: 'svg', ssr: true, width, height});
  try {
    chart.setOption({animation: false, ...option});
    return chart.renderToSVGString();
  } finally {
    chart.dispose();
  }
}

/**
 * The same as UTF-8 bytes, a part beside the JSON.
 * @param {{width?: number, height?: number, option: object}} spec as renderSvg's
 * @returns {Uint8Array}
 */
export function renderSvgBytes(spec) {
  return new TextEncoder().encode(renderSvg(spec));
}
