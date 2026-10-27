// Draws the benchmark charts in the browser from results.csv. The model and the Mermaid text mirror
// tools/perf_compare/charts.py; tools/test_perf_compare_sheet.py checks that both give the same chart text.
"use strict";

const SUITES = ["campaign", "multiplayer", "co-op"];
const SUITE_NAMES = { campaign: "Campaign", multiplayer: "Multiplayer", "co-op": "Co-op", overall: "Overall" };
const DEFAULT_SUITE = "campaign";

function parseCsv(text) {
  const records = [];
  let row = [], field = "", quoted = false;
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (quoted) {
      if (c === '"' && text[i + 1] === '"') { field += '"'; i++; }
      else if (c === '"') quoted = false;
      else field += c;
    } else if (c === '"') quoted = true;
    else if (c === ",") { row.push(field); field = ""; }
    else if (c === "\n" || c === "\r") {
      if (c === "\r" && text[i + 1] === "\n") i++;
      row.push(field); field = "";
      if (row.length > 1 || row[0] !== "") records.push(row);
      row = [];
    } else field += c;
  }
  if (field !== "" || row.length) { row.push(field); records.push(row); }
  const [header, ...body] = records;
  return body.map((values) => Object.fromEntries(header.map((name, i) => [name, values[i] ?? ""])));
}

const mean = (values) => values.reduce((a, b) => a + b, 0) / values.length;
const geometricMean = (values) => Math.exp(mean(values.map(Math.log)));
function matches(row, filters) {
  return Object.entries(filters).every(([column, wanted]) =>
    (Array.isArray(wanted) ? wanted : [wanted]).map(String).includes(row[column] ?? ""));
}
function niceCeiling(value) {
  if (value <= 0) return 1;
  const magnitude = 10 ** Math.floor(Math.log10(value));
  for (const step of [1, 1.5, 2, 2.5, 3, 4, 5, 6, 8, 10]) if (step * magnitude >= value) return step * magnitude;
  return 10 * magnitude;
}
const number = (value) => value.toFixed(1).replace(/\.0$/, "");
const entryKey = (entry) => [entry.port, entry.version, entry.build].join("@");

function suiteValue(group, query, suite) {
  const box = group.filter((row) => String(row.reference_box).toLowerCase() === "true");
  const unsupported = group.filter((row) => row.suite === suite && row.status === "not_supported");
  if (unsupported.length) return [null, unsupported[0].notes || "not supported", "not_supported"];
  if (!box.length) {
    const gone = group.filter((row) => row.status === "unavailable" && row.notes).map((row) => row.notes);
    return [null, gone.length ? gone[0] : "not measured on the reference box", "not_measured"];
  }
  const mine = box.filter((row) => row.suite === suite);
  if (!mine.length) return [null, `no ${suite} measurements yet`, "not_measured"];
  const usable = new Map();
  const candidates = mine.filter((row) => matches(row, query.filter) && query.metrics.every((m) => row[m] !== ""));
  candidates.sort((a, b) => (a.date < b.date ? -1 : a.date > b.date ? 1 : 0));
  for (const row of candidates) usable.set(row.window, row);
  if (!usable.size) return [null, query.no_data_reason || "no matching measurement", "not_measured"];
  const rows = [...usable.values()];
  return [{ avg: geometricMean(rows.map((r) => parseFloat(r.fps_avg))), low: geometricMean(rows.map((r) => parseFloat(r.fps_1pct_low))), rows }, "", ""];
}

function model(rows, query, suite) {
  const platformRows = rows.filter((row) => row.platform === query.platform);
  const groups = new Map();
  for (const row of platformRows) {
    const key = JSON.stringify([row.port, row.version, row.build]);
    if (!groups.has(key)) groups.set(key, []);
    groups.get(key).push(row);
  }
  const entries = [];
  for (const group of groups.values()) {
    const first = group[0];
    const entry = { port: first.port, port_name: first.port_name, version: first.version, build: first.build,
      label: first.label, date: group.map((r) => r.date).reduce((a, b) => (a > b ? a : b)) };
    if (suite === "overall") {
      const parts = SUITES.map((name) => suiteValue(group, query, name));
      const missing = SUITES.filter((name, i) => parts[i][0] === null);
      if (missing.length) {
        entry.value = null; entry.reason = "missing " + missing.join(", ");
        const kinds = new Set(parts.filter((p) => p[0] === null).map((p) => p[2]));
        entry.kind = kinds.size === 1 && kinds.has("not_supported") ? "not_supported" : "not_measured";
      }
      else {
        entry.value = { avg: geometricMean(parts.map((p) => p[0].avg)), low: geometricMean(parts.map((p) => p[0].low)) };
        entry.reason = ""; entry.kind = "";
      }
    } else [entry.value, entry.reason, entry.kind] = suiteValue(group, query, suite);
    entries.push(entry);
  }
  const order = query.port_order || [];
  const rank = (e) => (order.includes(e.port) ? order.indexOf(e.port) : order.length);
  entries.sort((a, b) => rank(a) - rank(b) || (a.port < b.port ? -1 : a.port > b.port ? 1 : 0)
    || (a.date < b.date ? 1 : a.date > b.date ? -1 : 0) || (a.version < b.version ? -1 : a.version > b.version ? 1 : 0)
    || (a.build < b.build ? -1 : a.build > b.build ? 1 : 0));
  const newest = new Map();
  for (const entry of entries) {
    if (entry.value !== null && (!newest.has(entry.port) || entry.date > newest.get(entry.port).date)) newest.set(entry.port, entry);
  }
  for (const entry of entries) entry.default = newest.get(entry.port) === entry;
  const reference = platformRows.find((row) => String(row.reference_box).toLowerCase() === "true");
  return { entries, box: reference ? reference.host : "" };
}

function drawn(built, selected) {
  return built.entries.filter((e) => e.value !== null && (selected ? selected.has(entryKey(e)) : e.default));
}

function headerText(chart, palette) {
  return chart.header.trim().replace("__PALETTE__", (palette || chart.palette || []).join(", "));
}

function chartSource(spec, built, suite, selected, palette) {
  const shown = drawn(built, selected);
  if (!shown.length) return null;
  const chart = spec.chart, rename = spec.rename || {};
  const labels = shown.map((e) => `${rename[e.port_name] || e.port_name} ${e.version}`.replaceAll('"', "'"));
  const averages = shown.map((e) => e.value.avg), lows = shown.map((e) => e.value.low);
  const yMax = chart.y_max ?? niceCeiling(Math.max(...averages, ...lows) * 1.08);
  const head = chart.horizontal ? "xychart-beta horizontal" : "xychart-beta";
  const title = `${SUITE_NAMES[suite]}: ${chart.title_unit || "uncapped fps"}, ${built.box || spec.box || ""}`.replaceAll('"', "'");
  return [headerText(chart, palette), head, `    title "${title}"`,
    `    x-axis "${chart.x_label || ""}" [${labels.map((l) => `"${l}"`).join(", ")}]`,
    `    y-axis "${chart.y_label || ""}" ${number(chart.y_min ?? 0)} --> ${number(yMax)}`,
    `    bar [${averages.map(number).join(", ")}]`, `    bar [${lows.map(number).join(", ")}]`].filter(Boolean).join("\n");
}

function legendText(spec) {
  const marks = spec.chart.marks || [];
  return spec.series_labels.map((name, i) => `${marks[i % marks.length]} ${name}`).join(" ");
}

function captionText(built, selected) {
  const shown = drawn(built, selected);
  if (!shown.length) return "";
  return `${built.box}. ` + shown.map((e) => `${e.label} (build ${e.build || "unknown"}, measured ${e.date})`).join("; ") + ".";
}

// Plain-language text for reason codes (docs/benchmarks/reasons.toml); unknown codes pass through.
function plainReason(reason, reasons) {
  if (Object.hasOwn(reasons.exact, reason)) return reasons.exact[reason];
  let match = /^no (.+) measurements yet$/.exec(reason);
  if (match) return reasons.patterns.no_suite_data.replace("{suite}", match[1]);
  match = /^missing (.+)$/.exec(reason);
  if (match) return reasons.patterns.missing_suites.replace("{suites}", match[1]);
  return reason;
}

if (typeof module !== "undefined") module.exports = { headerText, plainReason, parseCsv, model, chartSource, legendText, captionText, entryKey, SUITES };

// ---- Page ----
function readHash() {
  const params = new URLSearchParams(location.hash.slice(1));
  return params;
}

function writeHash(state, specs) {
  const params = new URLSearchParams();
  if (state.suite !== DEFAULT_SUITE) params.set("suite", state.suite);
  for (const spec of specs) {
    const built = state.models[spec.slug];
    const defaults = built.entries.filter((e) => e.default).map(entryKey).sort().join(",");
    const chosen = [...state.selected[spec.slug]].sort().join(",");
    if (chosen !== defaults) params.set(spec.slug, chosen);
  }
  const text = params.toString();
  history.replaceState(null, "", text ? "#" + decodeURIComponent(text).replaceAll(" ", "%20") : location.pathname + location.search);
}

function el(tag, attributes, ...children) {
  const node = document.createElement(tag);
  for (const [name, value] of Object.entries(attributes || {})) node.setAttribute(name, value);
  node.append(...children);
  return node;
}

// The site's dark tokens (docs/site/style.css) for the chart screen; the bar colors come from each chart's header.
function mermaidConfig(dark) {
  if (!dark) return { startOnLoad: false };
  const ink = "#e8f6fa", line = "#93adb6";
  return { startOnLoad: false, theme: "base", themeVariables: { xyChart: {
    backgroundColor: "#10232b", titleColor: ink, xAxisTitleColor: ink, xAxisLabelColor: ink, xAxisTickColor: line,
    xAxisLineColor: line, yAxisTitleColor: ink, yAxisLabelColor: ink, yAxisTickColor: line, yAxisLineColor: line } } };
}

// Bar colors come from the site's tokens (`--chart-avg`, `--chart-low` in style.css), so light and dark both work.
function themePalette() {
  const style = getComputedStyle(document.documentElement);
  return ["--chart-avg", "--chart-low"].map((name) => style.getPropertyValue(name).trim());
}

// Mermaid's xychart draws its content flush to the edge of its canvas (the last tick label overhangs it). After
// rendering, this fits the view box to the content plus an even margin on all four sides. `svg` must be in the
// document so `getBBox` works. (Mermaid 11.12 has no gridline option, so the charts have none.)
function finishChart(svg, pad = 22) {
  const main = svg.querySelector("g.main");
  if (!main) return svg;
  const background = main.querySelector("rect.background");
  let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
  for (const child of main.children) {
    if (child === background || !child.getBBox) continue;
    const box = child.getBBox();
    if (!box.width && !box.height) continue;
    x0 = Math.min(x0, box.x); y0 = Math.min(y0, box.y); x1 = Math.max(x1, box.x + box.width); y1 = Math.max(y1, box.y + box.height);
  }
  if (Number.isFinite(x0)) {
    const view = [Math.floor(x0 - pad), Math.floor(y0 - pad), Math.ceil(x1 - x0 + 2 * pad), Math.ceil(y1 - y0 + 2 * pad)];
    svg.setAttribute("viewBox", view.join(" "));
    svg.style.maxWidth = view[2] + "px";
    if (background) for (const [name, value] of [["x", view[0]], ["y", view[1]], ["width", view[2]], ["height", view[3]]]) background.setAttribute(name, value);
  }
  return svg;
}

// For the pre-rendered fallback: mount the SVG text off screen, finish it and give back its markup.

// For the pre-rendered fallback: mount the SVG text off screen, finish it and give back its markup.
function finishChartSvg(text) {
  const stage = document.createElement("div");
  stage.style.cssText = "position:absolute;left:-10000px;top:0;width:900px;visibility:hidden";
  stage.innerHTML = text;
  document.body.append(stage);
  const svg = finishChart(stage.querySelector("svg"));
  const markup = svg.outerHTML;
  stage.remove();
  return markup;
}

let renderCount = 0;
async function drawChart(container, spec, built, suite, selected) {
  const text = chartSource(spec, built, suite, selected, themePalette());
  container.replaceChildren();
  if (text === null) {
    const anyData = built.entries.some((e) => e.value !== null);
    container.append(el("p", { class: "bench-empty" }, !built.entries.length ? spec.empty
      : anyData ? "Nothing is ticked." : "No build has data for this suite yet."));
    return;
  }
  try {
    // Mermaid measures text where it renders, so use a fixed-width off-screen box rather than the phone-width page.
    const stage = el("div", { style: "position:absolute;left:-10000px;top:0;width:900px;visibility:hidden" });
    document.body.append(stage);
    let svg;
    try { ({ svg } = await mermaid.render("bench-chart-" + ++renderCount, text, stage)); } finally { stage.remove(); }
    container.innerHTML = svg;
    finishChart(container.querySelector("svg"));
  } catch (error) {
    container.append(el("pre", {}, text));
  }
}

// The legend uses swatches in the chart's own colors (CSS tokens), not emoji.
function drawLegend(node, spec, show) {
  node.replaceChildren();
  if (!show) return;
  spec.series_labels.forEach((name, i) => {
    node.append(el("span", { class: "swatch " + (i ? "low" : "avg"), "aria-hidden": "true" }), " " + name + (i < spec.series_labels.length - 1 ? "  " : ""));
  });
}

function drawList(list, spec, built, state) {
  const reasons = state.reasons;
  list.hidden = !built.entries.length;
  list.replaceChildren(el("legend", {}, "Builds"));
  for (const entry of built.entries) {
    const key = entryKey(entry);
    const detail = `build ${entry.build || "unknown"}, ${entry.date}`;
    const input = el("input", { type: "checkbox", value: key });
    input.checked = state.selected[spec.slug].has(key);
    if (entry.value === null) {
      input.disabled = true;
      input.checked = false;
    }
    const word = entry.kind === "not_supported" ? "Not supported" : "Not measured";
    const text = entry.value === null ? `${entry.label} (${detail}). ${word}: ${plainReason(entry.reason, reasons)}` : `${entry.label} (${detail})`;
    const label = el("label", { class: entry.value === null ? "off" : "" }, input, " ", text);
    input.addEventListener("change", () => {
      if (input.checked) state.selected[spec.slug].add(key); else state.selected[spec.slug].delete(key);
      state.refresh(spec);
    });
    list.append(label);
  }
}

async function start() {
  const root = document.getElementById("bench-root");
  if (!root || typeof mermaid === "undefined") return;
  const specs = JSON.parse(document.getElementById("chart-specs").textContent);
  const reasons = JSON.parse(document.getElementById("chart-reasons").textContent);
  const text = await (await fetch(root.dataset.csv)).text();
  const rows = parseCsv(text);
  const params = readHash();
  const state = { suite: SUITES.concat("overall").includes(params.get("suite")) ? params.get("suite") : DEFAULT_SUITE,
    models: {}, selected: {}, refresh: null, reasons };
  const dark = window.matchMedia("(prefers-color-scheme: dark)");
  const theme = () => mermaid.initialize(mermaidConfig(dark.matches));
  theme();
  const sections = Object.fromEntries(specs.map((spec) => [spec.slug, document.querySelector(`[data-platform="${spec.slug}"]`)]));

  function recompute(spec, keepSelection) {
    state.models[spec.slug] = model(rows, spec.query, state.suite);
    const built = state.models[spec.slug];
    const requested = params.get(spec.slug);
    if (!keepSelection) {
      state.selected[spec.slug] = new Set(requested !== null && !state.touched
        ? requested.split(",").filter(Boolean) : built.entries.filter((e) => e.default).map(entryKey));
    }
  }
  async function refresh(spec) {
    const built = state.models[spec.slug], section = sections[spec.slug];
    await drawChart(section.querySelector(".bench-chart"), spec, built, state.suite, state.selected[spec.slug]);
    drawList(section.querySelector(".bench-rows"), spec, built, state);
    drawLegend(section.querySelector(".bench-legend"), spec, built.entries.length > 0);
    section.querySelector(".bench-caption").textContent = captionText(built, state.selected[spec.slug]);
    writeHash(state, specs);
  }
  state.refresh = refresh;
  dark.addEventListener("change", async () => { theme(); for (const spec of specs) await refresh(spec); });
  const switcher = document.querySelectorAll('input[name="suite"]');
  for (const input of switcher) {
    input.checked = input.value === state.suite;
    input.addEventListener("change", async () => {
      state.suite = input.value;
      state.touched = true;
      for (const spec of specs) recompute(spec, false);
      for (const spec of specs) await refresh(spec);
    });
  }
  for (const spec of specs) recompute(spec, false);
  for (const spec of specs) await refresh(spec);
}

if (typeof document !== "undefined") start().catch((error) => console.error(error));
