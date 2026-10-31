const $ = (id) => document.getElementById(id);
const KEYS = { positions: "ng_positions", games: "ng_games", analyses: "ng_analyses" };

function setStatus(message, isError) {
  $("status").textContent = message || "";
  $("status").style.color = isError ? "#fda4af" : "#a5f3fc";
}
function slug(value) {
  return (value || "untitled").toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "").slice(0, 48) || "untitled";
}
function validateFenShape(fen) {
  const fields = fen.trim().split(/\s+/);
  if (fields.length !== 6) return "FEN must contain six fields.";
  const ranks = fields[0].split("/");
  if (ranks.length !== 8) return "FEN board must have eight ranks.";
  for (const rank of ranks) {
    if (!/^[prnbqkPRNBQK1-8]+$/.test(rank)) return "FEN contains an invalid board character.";
    let count = 0;
    for (const ch of rank) count += /^[1-8]$/.test(ch) ? Number(ch) : 1;
    if (count !== 8) return "Each FEN rank must describe eight squares.";
  }
  if (!["w", "b"].includes(fields[1])) return "FEN side-to-move must be w or b.";
  if (!/^(?:-|K?Q?k?q?)$/.test(fields[2])) return "FEN castling field is invalid.";
  if (!/^(?:-|[a-h][36])$/.test(fields[3])) return "FEN en-passant field is invalid.";
  if (!/^\d+$/.test(fields[4]) || !/^\d+$/.test(fields[5])) return "FEN move counters must be numbers.";
  return "";
}
async function getMemory() {
  const data = await chrome.storage.local.get(Object.values(KEYS));
  return {
    positions: data[KEYS.positions] || [],
    games: data[KEYS.games] || [],
    analyses: data[KEYS.analyses] || []
  };
}
async function putMemory(memory) {
  await chrome.storage.local.set({
    [KEYS.positions]: memory.positions,
    [KEYS.games]: memory.games,
    [KEYS.analyses]: memory.analyses
  });
}
function render(memory) {
  $("counts").textContent = memory.positions.length + " positions · " + memory.games.length + " games";
  const host = $("memoryList");
  host.textContent = "";
  const rows = [
    ...memory.positions.slice().reverse().map((x) => ({ type: "position", item: x })),
    ...memory.games.slice().reverse().map((x) => ({ type: "game", item: x }))
  ].slice(0, 12);
  if (!rows.length) {
    const empty = document.createElement("p");
    empty.className = "empty";
    empty.textContent = "Your saved positions and games will appear here.";
    host.appendChild(empty);
    return;
  }
  for (const row of rows) {
    const wrap = document.createElement("div");
    wrap.className = "memory-item";
    const title = document.createElement("div");
    title.className = "memory-title";
    title.textContent = (row.type === "position" ? "♟ " : "▤ ") + (row.item.name || row.item.title || "Untitled");
    const meta = document.createElement("div");
    meta.className = "memory-meta";
    meta.textContent = row.type === "position" ? row.item.fen : "PGN · " + (row.item.pgn || "").length + " characters";
    wrap.append(title, meta);
    if (row.type === "position") {
      const load = document.createElement("button");
      load.className = "secondary";
      load.textContent = "Load in lab";
      load.addEventListener("click", () => {
        $("fen").value = row.item.fen;
        $("positionName").value = row.item.name || "";
        setStatus("Position loaded into the lab.");
      });
      wrap.appendChild(load);
    }
    host.appendChild(wrap);
  }
}
async function refresh() { const memory = await getMemory(); render(memory); }

$("savePosition").addEventListener("click", async () => {
  const fen = $("fen").value.trim();
  const error = validateFenShape(fen);
  if (error) { setStatus(error, true); return; }
  const memory = await getMemory();
  memory.positions.push({
    id: crypto.randomUUID(),
    name: $("positionName").value.trim() || "Saved position",
    fen,
    source: "manual_fen",
    added_at: new Date().toISOString()
  });
  await putMemory(memory);
  render(memory);
  setStatus("Position saved in local extension memory.");
});

$("saveGame").addEventListener("click", async () => {
  const pgn = $("pgn").value.trim();
  if (pgn.length < 12 || !/\d+\./.test(pgn)) {
    setStatus("Paste a completed game's PGN, including its move list.", true);
    return;
  }
  const memory = await getMemory();
  memory.games.push({
    id: crypto.randomUUID(),
    title: $("gameName").value.trim() || "Imported completed game",
    pgn,
    source: "user_imported_pgn",
    added_at: new Date().toISOString()
  });
  await putMemory(memory);
  $("pgn").value = "";
  render(memory);
  setStatus("Game saved. Its positions will be extracted when you build the memory dataset.");
});

$("analyse").addEventListener("click", () => {
  const fen = $("fen").value.trim();
  const error = validateFenShape(fen);
  if (error) { setStatus(error, true); return; }
  if (!$("postgameConfirm").checked) { setStatus("Engine analysis is for completed games and study positions only.", true); return; }
  const depth = Math.max(1, Math.min(24, Number($("depth").value) || 12));
  $("analyse").disabled = true;
  $("analysis").textContent = "Starting the local NextGen engine…";
  setStatus("Analysing locally. Deeper searches may take longer.");
  chrome.runtime.sendNativeMessage("com.nextgen.chess_engine", {
    type: "analyse",
    fen,
    depth,
    useNnue: $("useNnue").checked
  }, async (response) => {
    $("analyse").disabled = false;
    if (chrome.runtime.lastError) {
      const msg = chrome.runtime.lastError.message || "Native engine connection failed.";
      $("analysis").textContent = msg + "\nRun browser-extension/install-native-host.ps1 after loading the extension.";
      setStatus("Engine host is not connected.", true);
      return;
    }
    if (!response || !response.ok) {
      $("analysis").textContent = response && response.error ? response.error : "No response from the engine.";
      setStatus("Analysis failed.", true);
      return;
    }
    $("analysis").textContent = "Best move: " + response.bestmove +
      (response.info ? "\n" + response.info : "") +
      (response.nnue ? "\nExperimental NNUE loaded." : "");
    const memory = await getMemory();
    memory.analyses.push({
      id: crypto.randomUUID(),
      fen,
      depth,
      bestmove: response.bestmove,
      info: response.info || "",
      nnue: !!response.nnue,
      added_at: new Date().toISOString()
    });
    await putMemory(memory);
    render(memory);
    setStatus("Analysis saved to memory.");
  });
});

async function writeFile(directory, name, content) {
  const handle = await directory.getFileHandle(name, { create: true });
  const writable = await handle.createWritable();
  await writable.write(content);
  await writable.close();
}
$("exportMemory").addEventListener("click", async () => {
  if (!window.showDirectoryPicker) {
    setStatus("This Chrome version does not support folder export. Update Chrome and try again.", true);
    return;
  }
  try {
    const parent = await window.showDirectoryPicker({ id: "nextgen-memory-export", mode: "readwrite" });
    const root = await parent.getDirectoryHandle("NextGen-Chess-Memory", { create: true });
    const gamesDir = await root.getDirectoryHandle("games", { create: true });
    const positionsDir = await root.getDirectoryHandle("positions", { create: true });
    const analysesDir = await root.getDirectoryHandle("analyses", { create: true });
    const memory = await getMemory();

    const allPgn = memory.games.map((game) => game.pgn.trim()).filter(Boolean).join("\n\n");
    await writeFile(gamesDir, "all_games.pgn", allPgn ? allPgn + "\n" : "");
    for (let i = 0; i < memory.games.length; i++) {
      const game = memory.games[i];
      const filename = String(i + 1).padStart(4, "0") + "-" + slug(game.title) + ".pgn";
      await writeFile(gamesDir, filename, game.pgn.trim() + "\n");
    }
    const positionRows = memory.positions.map((p) => ({
      fen: p.fen, name: p.name, source: p.source, added_at: p.added_at
    }));
    await writeFile(positionsDir, "manual_positions.jsonl",
      positionRows.map((x) => JSON.stringify(x)).join("\n") + (positionRows.length ? "\n" : ""));
    await writeFile(analysesDir, "engine_analyses.jsonl",
      memory.analyses.map((x) => JSON.stringify(x)).join("\n") + (memory.analyses.length ? "\n" : ""));
    await writeFile(root, "summary.json", JSON.stringify({
      schema: "nextgen-chess-memory-v1",
      exported_at: new Date().toISOString(),
      positions_saved: memory.positions.length,
      games_saved: memory.games.length,
      analyses_saved: memory.analyses.length,
      note: "Use scripts/build_extension_memory_dataset.py to extract legal positions from imported PGNs."
    }, null, 2) + "\n");
    await writeFile(root, "README.txt",
      "NextGen Chess Memory\n\n" +
      "games/all_games.pgn contains imported completed games.\n" +
      "positions/manual_positions.jsonl contains manually saved FENs.\n" +
      "analyses/engine_analyses.jsonl contains offline NextGen analyses.\n\n" +
      "From the NextGen repository root, run:\n" +
      "  py -m pip install python-chess\n" +
      "  py scripts/build_extension_memory_dataset.py \"<this folder>\\"\n\n" +
      "This creates positions/positions.jsonl from the PGN move histories.\n" +
      "No live-game moves are collected or played automatically.\n");
    setStatus("Exported NextGen-Chess-Memory folder successfully.");
  } catch (error) {
    if (error && error.name === "AbortError") return;
    setStatus("Folder export failed: " + (error.message || error), true);
  }
});

$("clearMemory").addEventListener("click", async () => {
  if (!confirm("Delete all positions, imported games and analyses stored by this extension?")) return;
  await chrome.storage.local.remove(Object.values(KEYS));
  await refresh();
  setStatus("Extension memory cleared. Existing exported folders are unchanged.");
});

refresh().catch((error) => setStatus("Could not load extension memory: " + error.message, true));
