#!/usr/bin/env node
/*
 * copper-libs — MCP server exposing locally installed KiCad symbol &
 * footprint libraries, plus tools to repair/install library tables.
 *
 * Part of kicad-copper (GPLv3). No external dependencies; speaks the MCP
 * stdio transport (newline-delimited JSON-RPC 2.0) by hand.
 *
 * Tools:
 *   list_libraries        — every lib in sym-lib-table / fp-lib-table, with
 *                           resolved paths and whether they exist on disk
 *   search_symbols        — substring/regex search across .kicad_sym files
 *   search_footprints     — search .kicad_mod names across .pretty dirs
 *   get_symbol_info       — pins/properties/description for one lib_id
 *   repair_library_tables — detect an existing KiCad install (e.g. 9.0 in
 *                           Program Files) and point the newest config's
 *                           env vars + lib tables at it. Instant, no
 *                           download.
 *   install_libraries     — git clone --depth 1 the official KiCad symbol
 *                           (and optionally footprint) libraries and
 *                           register them in the lib tables.
 */

import fs from "node:fs";
import path from "node:path";
import os from "node:os";
import readline from "node:readline";
import { execFileSync } from "node:child_process";

// ───────────────────────── KiCad config discovery ─────────────────────────

function configRoot() {
    if (process.platform === "win32")
        return path.join(process.env.APPDATA ?? path.join(os.homedir(), "AppData", "Roaming"), "kicad");
    if (process.platform === "darwin")
        return path.join(os.homedir(), "Library", "Preferences", "kicad");
    return path.join(process.env.XDG_CONFIG_HOME ?? path.join(os.homedir(), ".config"), "kicad");
}

/** Newest version dir (e.g. "10.99" > "9.0") under the kicad config root. */
function newestConfigDir() {
    const root = configRoot();
    let dirs = [];
    try {
        dirs = fs.readdirSync(root).filter(d => /^\d+\.\d+$/.test(d) &&
            fs.statSync(path.join(root, d)).isDirectory());
    } catch {
        return null;
    }
    if (!dirs.length) return null;
    dirs.sort((a, b) => parseFloat(b) - parseFloat(a));
    return path.join(root, dirs[0]);
}

/** Env vars from kicad_common.json plus process env. */
function kicadEnvVars(cfgDir) {
    const vars = {};
    try {
        const common = JSON.parse(fs.readFileSync(path.join(cfgDir, "kicad_common.json"), "utf8"));
        Object.assign(vars, common?.environment?.vars ?? {});
    } catch { /* no config yet */ }
    for (const [k, v] of Object.entries(process.env))
        if (/^KICAD\d*_/.test(k)) vars[k] ??= v;
    return vars;
}

/** Detect installed KiCad share dirs (Windows Program Files, unix prefixes). */
function detectInstalls() {
    const candidates = [];
    if (process.platform === "win32") {
        for (const pf of ["C:\\Program Files\\KiCad", "C:\\Program Files (x86)\\KiCad"]) {
            try {
                for (const ver of fs.readdirSync(pf)) {
                    const share = path.join(pf, ver, "share", "kicad");
                    if (fs.existsSync(path.join(share, "symbols")))
                        candidates.push({ version: ver, share });
                }
            } catch { /* not installed there */ }
        }
    } else {
        for (const share of ["/usr/share/kicad", "/usr/local/share/kicad",
                             "/Applications/KiCad/KiCad.app/Contents/SharedSupport"])
            if (fs.existsSync(path.join(share, "symbols")))
                candidates.push({ version: "system", share });
    }
    candidates.sort((a, b) => parseFloat(b.version) - parseFloat(a.version) || 0);
    return candidates;
}

// ───────────────────────── lib-table parsing ─────────────────────────

/** Minimal s-expression tokenizer good enough for lib tables. */
function parseLibTable(file) {
    let text;
    try { text = fs.readFileSync(file, "utf8"); } catch { return null; }
    const libs = [];
    // values may be quoted ("Audio_Module") or bare tokens (Audio_Module)
    const val = `(?:"((?:[^"\\\\]|\\\\.)*)"|([^()\\s"]+))`;
    const re = new RegExp(
        `\\(lib\\s*\\(name\\s+${val}\\)\\s*\\(type\\s+${val}\\)\\s*\\(uri\\s+${val}\\)`, "g");
    let m;
    while ((m = re.exec(text)))
        libs.push({ name: m[1] ?? m[2], type: m[3] ?? m[4], uri: m[5] ?? m[6] });
    return libs;
}

function expandVars(uri, vars) {
    return uri.replace(/\$\{([^}]+)\}/g, (_, k) => vars[k] ?? `\${${k}}`);
}

function loadTables() {
    const cfg = newestConfigDir();
    if (!cfg) return { error: "no KiCad config directory found", configDir: null };
    const vars = kicadEnvVars(cfg);
    const sym = parseLibTable(path.join(cfg, "sym-lib-table")) ?? [];
    const fp = parseLibTable(path.join(cfg, "fp-lib-table")) ?? [];
    const annotate = l => {
        const resolved = expandVars(l.uri, vars);
        return { ...l, resolved, exists: !resolved.includes("${") && fs.existsSync(resolved) };
    };
    return { configDir: cfg, vars, symbols: sym.map(annotate), footprints: fp.map(annotate) };
}

// ───────────────────────── symbol/footprint search ─────────────────────────

const symbolCache = new Map(); // file -> { mtime, names: [] }

function symbolNames(file) {
    let st;
    try { st = fs.statSync(file); } catch { return []; }
    const hit = symbolCache.get(file);
    if (hit && hit.mtime === st.mtimeMs) return hit.names;
    const names = [];
    const text = fs.readFileSync(file, "utf8");
    // top-level symbols only: indented exactly one tab/2-spaces under (kicad_symbol_lib
    const re = /^[\t ]{1,2}\(symbol\s+"((?:[^"\\]|\\.)*)"/gm;
    let m;
    while ((m = re.exec(text)))
        if (!/_\d+_\d+$/.test(m[1])) names.push(m[1]);
    symbolCache.set(file, { mtime: st.mtimeMs, names });
    return names;
}

function searchSymbols({ query, library, limit = 50 }) {
    const t = loadTables();
    if (t.error) return t;
    const q = query.toLowerCase();
    const out = [];
    for (const lib of t.symbols) {
        if (library && lib.name !== library) continue;
        if (!lib.exists) continue;
        for (const n of symbolNames(lib.resolved)) {
            if (n.toLowerCase().includes(q)) {
                out.push(`${lib.name}:${n}`);
                if (out.length >= limit) return { matches: out, truncated: true };
            }
        }
    }
    return { matches: out, truncated: false };
}

function searchFootprints({ query, library, limit = 50 }) {
    const t = loadTables();
    if (t.error) return t;
    const q = query.toLowerCase();
    const out = [];
    for (const lib of t.footprints) {
        if (library && lib.name !== library) continue;
        if (!lib.exists) continue;
        let files = [];
        try { files = fs.readdirSync(lib.resolved).filter(f => f.endsWith(".kicad_mod")); }
        catch { continue; }
        for (const f of files) {
            const n = f.slice(0, -10);
            if (n.toLowerCase().includes(q)) {
                out.push(`${lib.name}:${n}`);
                if (out.length >= limit) return { matches: out, truncated: true };
            }
        }
    }
    return { matches: out, truncated: false };
}

function getSymbolInfo({ lib_id }) {
    const [libName, symName] = String(lib_id).split(":");
    if (!libName || !symName) return { error: "lib_id must be 'Library:Symbol'" };
    const t = loadTables();
    if (t.error) return t;
    const lib = t.symbols.find(l => l.name === libName);
    if (!lib) return { error: `library '${libName}' not in sym-lib-table` };
    if (!lib.exists) return { error: `library file missing: ${lib.resolved}` };
    const text = fs.readFileSync(lib.resolved, "utf8");
    const start = text.search(new RegExp(`^[\\t ]{1,2}\\(symbol\\s+"${symName.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")}"`, "m"));
    if (start < 0) return { error: `symbol '${symName}' not found in ${libName}` };
    // walk parens to find the end of this symbol block
    let depth = 0, i = text.indexOf("(", start), end = i;
    for (; i < text.length; i++) {
        if (text[i] === "(") depth++;
        else if (text[i] === ")") { depth--; if (depth === 0) { end = i + 1; break; } }
    }
    const block = text.slice(start, end);
    const prop = name => {
        const m = block.match(new RegExp(`\\(property\\s+"${name}"\\s+"((?:[^"\\\\]|\\\\.)*)"`));
        return m ? m[1] : null;
    };
    const pins = [...block.matchAll(/\(pin\s+(\w+)\s+\w+\s*[\s\S]*?\(name\s+"((?:[^"\\]|\\.)*)"[\s\S]*?\(number\s+"((?:[^"\\]|\\.)*)"/g)]
        .map(m => ({ number: m[3], name: m[2], type: m[1] }));
    return {
        lib_id,
        description: prop("Description"),
        datasheet: prop("Datasheet"),
        footprint: prop("Footprint"),
        keywords: prop("ki_keywords"),
        fp_filters: prop("ki_fp_filters"),
        pin_count: pins.length,
        pins: pins.slice(0, 100),
    };
}

// ───────────────────────── repair / install ─────────────────────────

function writeLibTable(file, kind, entries) {
    const rows = entries.map(e =>
        `  (lib (name "${e.name}")(type "${e.type}")(uri "${e.uri}")(options "")(descr "${e.descr ?? ""}"))`);
    fs.writeFileSync(file, `(${kind}\n  (version 7)\n${rows.join("\n")}\n)\n`);
}

function tableEntriesFromDir(dir, ext, type) {
    return fs.readdirSync(dir)
        .filter(f => f.endsWith(ext))
        .map(f => ({
            name: f.slice(0, -ext.length),
            type,
            uri: path.join(dir, f).replace(/\\/g, "\\\\"),
        }));
}

function backupIfExists(file) {
    if (fs.existsSync(file)) fs.copyFileSync(file, file + ".copper-backup");
}

function repairLibraryTables({ dry_run = false } = {}) {
    const cfg = newestConfigDir();
    if (!cfg) return { error: "no KiCad config directory found — launch eeschema once first" };
    const installs = detectInstalls();
    if (!installs.length)
        return { error: "no KiCad install with libraries found — use install_libraries instead" };
    const { share, version } = installs[0];
    const symDir = path.join(share, "symbols");
    const fpDir = path.join(share, "footprints");
    const plan = { configDir: cfg, source: share, sourceVersion: version, dry_run };

    const symEntries = tableEntriesFromDir(symDir, ".kicad_sym", "KiCad");
    plan.symbolLibs = symEntries.length;
    let fpEntries = [];
    if (fs.existsSync(fpDir)) {
        fpEntries = fs.readdirSync(fpDir).filter(f => f.endsWith(".pretty")).map(f => ({
            name: f.slice(0, -7), type: "KiCad",
            uri: path.join(fpDir, f).replace(/\\/g, "\\\\"),
        }));
    }
    plan.footprintLibs = fpEntries.length;

    if (!dry_run) {
        backupIfExists(path.join(cfg, "sym-lib-table"));
        backupIfExists(path.join(cfg, "fp-lib-table"));
        writeLibTable(path.join(cfg, "sym-lib-table"), "sym_lib_table", symEntries);
        if (fpEntries.length)
            writeLibTable(path.join(cfg, "fp-lib-table"), "fp_lib_table", fpEntries);
        plan.note = "tables rewritten with absolute paths (backups: *.copper-backup). Restart eeschema.";
    }
    return plan;
}

function installLibraries({ dest, include_footprints = false } = {}) {
    const cfg = newestConfigDir();
    if (!cfg) return { error: "no KiCad config directory found — launch eeschema once first" };
    dest ??= path.join(os.homedir(), "Documents", "KiCad", "copper-libs");
    fs.mkdirSync(dest, { recursive: true });

    const clone = (repo, dir) => {
        if (fs.existsSync(path.join(dir, ".git"))) {
            execFileSync("git", ["-C", dir, "pull", "--ff-only"], { stdio: "pipe", timeout: 600000 });
            return "updated";
        }
        execFileSync("git", ["clone", "--depth", "1",
            `https://gitlab.com/kicad/libraries/${repo}.git`, dir],
            { stdio: "pipe", timeout: 1800000 });
        return "cloned";
    };

    const result = { dest, configDir: cfg };
    const symDir = path.join(dest, "kicad-symbols");
    result.symbols = clone("kicad-symbols", symDir);
    backupIfExists(path.join(cfg, "sym-lib-table"));
    writeLibTable(path.join(cfg, "sym-lib-table"), "sym_lib_table",
        tableEntriesFromDir(symDir, ".kicad_sym", "KiCad"));

    if (include_footprints) {
        const fpDir = path.join(dest, "kicad-footprints");
        result.footprints = clone("kicad-footprints", fpDir);
        backupIfExists(path.join(cfg, "fp-lib-table"));
        writeLibTable(path.join(cfg, "fp-lib-table"), "fp_lib_table",
            fs.readdirSync(fpDir).filter(f => f.endsWith(".pretty")).map(f => ({
                name: f.slice(0, -7), type: "KiCad",
                uri: path.join(fpDir, f).replace(/\\/g, "\\\\"),
            })));
    }
    result.note = "library tables rewritten (backups: *.copper-backup). Restart eeschema.";
    return result;
}

// ───────────────────────── MCP plumbing ─────────────────────────

const TOOLS = [
    {
        name: "list_libraries",
        description: "List every KiCad symbol and footprint library registered in the user's global lib tables, with resolved paths and whether each exists on disk. Start here to diagnose 'Library not found' errors.",
        inputSchema: { type: "object", properties: {} },
        handler: () => loadTables(),
    },
    {
        name: "search_symbols",
        description: "Search locally installed KiCad symbol libraries by substring (e.g. 'rp2040', 'lm358'). Returns lib_ids usable in PLACE_COMPONENT ops.",
        inputSchema: {
            type: "object",
            properties: {
                query: { type: "string", description: "case-insensitive substring" },
                library: { type: "string", description: "restrict to one library name" },
                limit: { type: "number", default: 50 },
            },
            required: ["query"],
        },
        handler: searchSymbols,
    },
    {
        name: "search_footprints",
        description: "Search locally installed KiCad footprint libraries (.pretty dirs) by substring (e.g. 'SOIC-8', '0805').",
        inputSchema: {
            type: "object",
            properties: {
                query: { type: "string" },
                library: { type: "string" },
                limit: { type: "number", default: 50 },
            },
            required: ["query"],
        },
        handler: searchFootprints,
    },
    {
        name: "get_symbol_info",
        description: "Get pins, description, default footprint and footprint filters for one symbol, e.g. 'Device:R' or 'MCU_RaspberryPi:RP2040'.",
        inputSchema: {
            type: "object",
            properties: { lib_id: { type: "string", description: "'Library:Symbol'" } },
            required: ["lib_id"],
        },
        handler: getSymbolInfo,
    },
    {
        name: "repair_library_tables",
        description: "Fix 'Library not found in library table' by detecting an existing KiCad install (e.g. KiCad 9 in Program Files) and rewriting the newest config's sym-lib-table/fp-lib-table to point at it with absolute paths. Instant, no download. Backs up existing tables.",
        inputSchema: {
            type: "object",
            properties: { dry_run: { type: "boolean", description: "report what would be done without writing" } },
        },
        handler: repairLibraryTables,
    },
    {
        name: "install_libraries",
        description: "Download the official KiCad symbol libraries (git clone --depth 1 from gitlab.com/kicad/libraries) and register them in the global lib tables. Footprints optional (~1 GB).",
        inputSchema: {
            type: "object",
            properties: {
                dest: { type: "string", description: "install dir (default: ~/Documents/KiCad/copper-libs)" },
                include_footprints: { type: "boolean", default: false },
            },
        },
        handler: installLibraries,
    },
];

function send(msg) {
    process.stdout.write(JSON.stringify(msg) + "\n");
}

const rl = readline.createInterface({ input: process.stdin });
rl.on("line", line => {
    line = line.trim();
    if (!line) return;
    let req;
    try { req = JSON.parse(line); } catch { return; }
    const { id, method, params } = req;
    try {
        if (method === "initialize") {
            send({ jsonrpc: "2.0", id, result: {
                protocolVersion: params?.protocolVersion ?? "2024-11-05",
                capabilities: { tools: {} },
                serverInfo: { name: "copper-libs", version: "0.1.0" },
            }});
        } else if (method === "notifications/initialized") {
            // notification — no response
        } else if (method === "tools/list") {
            send({ jsonrpc: "2.0", id, result: {
                tools: TOOLS.map(({ name, description, inputSchema }) => ({ name, description, inputSchema })),
            }});
        } else if (method === "tools/call") {
            const tool = TOOLS.find(t => t.name === params?.name);
            if (!tool) {
                send({ jsonrpc: "2.0", id, error: { code: -32602, message: `unknown tool: ${params?.name}` } });
                return;
            }
            const out = tool.handler(params?.arguments ?? {});
            send({ jsonrpc: "2.0", id, result: {
                content: [{ type: "text", text: JSON.stringify(out, null, 2) }],
                isError: Boolean(out && out.error),
            }});
        } else if (id !== undefined) {
            send({ jsonrpc: "2.0", id, error: { code: -32601, message: `method not found: ${method}` } });
        }
    } catch (e) {
        if (id !== undefined)
            send({ jsonrpc: "2.0", id, error: { code: -32603, message: String(e?.message ?? e) } });
    }
});
