// 无头驱动：拼接 JSMinesweeper 核心源码，构造真实 expert 盘面并自动对弈，
// 观察参考求解器在 midgame（解数 > BFDA 阈值）时的真实决策日志。
"use strict";
const fs = require("fs");
const path = require("path");

const BASE = path.join(__dirname, "jsminesweeper", "JSMinesweeper-master", "Minesweeper");

// ---------- 1. 拼接前置 stub ----------
let code = "";
code += `
const ACTION_CLEAR = 1;
const ACTION_FLAG = 2;
const ACTION_CHORD = 3;
const BOMB = 9;   // 主程序 main.js 里的全局常量（BFDA 使用）
function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }
function showMessage(t) { console.log("[MSG] " + t); }
let analysisMode = false;   // 浏览器全局，headless 下按 play 模式
`;

// ---------- 2. 拼接核心源码 ----------
const files = [
  "Utility/Binomial.js",
  "Utility/Compression.js",
  "Utility/PrimeSieve.js",
  "client/Tile.js",
  "client/Board.js",
  "client/SolutionCounter.js",
  "client/solver_probability_engine.js",
  "client/Brute_force.js",
  "client/BruteForceAnalysis.js",
  "client/EfficiencyHelper.js",
  "client/FiftyFiftyHelper.js",
  "client/LongTermRiskHelper.js",
  "client/solver_main.js",
];
for (const f of files) {
  const p = path.join(BASE, f);
  code += "\n// ===== " + f + " =====\n" + fs.readFileSync(p, "utf8") + "\n";
}

// ---------- 3. 驱动代码 ----------
code += `
// ===== globals init (after class definitions) =====
const MAX_BINOMIAL_N = 65000;
const BINOMIAL = new Binomial(MAX_BINOMIAL_N, 500);
const binomialCache = new BinomialCache(5000, 500, BINOMIAL);
// 参考求解器 bug 实证：secondarySafetyAnalysis 里 safetyTally 可为 0 → divideBigInt 除零 RangeError
// （seed=3 natural board，见 ref_play_3.txt 失败前的日志）。此处给它打补丁以便继续对弈统计。
const origDivideBigInt = divideBigInt;
divideBigInt = (a, b, dp) => (b === BigInt(0) ? 0 : origDivideBigInt(a, b, dp));
solver(null, {});   // 初始化 solver.countSolutions = countSolutions

// ===== driver =====
const fs = require("fs");
const path = require("path");
let board;   // 参考代码 LongTermRiskHelper.getBoxInfluence 引用裸全局 board
const W = 30, H = 16, MINES = 99;

// Xorshift64 定种子（种子不同则盘面不同）
let xorshiftState = BigInt(0xC0FFEE12345);
function xorshift64() {
  xorshiftState ^= xorshiftState << BigInt(13); xorshiftState &= ((BigInt(1) << BigInt(64)) - BigInt(1));
  xorshiftState ^= xorshiftState >> BigInt(7);
  xorshiftState ^= xorshiftState << BigInt(17); xorshiftState &= ((BigInt(1) << BigInt(64)) - BigInt(1));
  return xorshiftState;
}

function makeBoard(seed) {
  xorshiftState = seed;
  const board = new Board(0, W, H, MINES, "", "safe");
  // 首点 (3,3) 安全区
  const safe = new Set();
  for (let dx = -1; dx <= 1; dx++) for (let dy = -1; dy <= 1; dy++) {
    const x = 3 + dx, y = 3 + dy;
    if (x >= 0 && x < W && y >= 0 && y < H) safe.add(y * W + x);
  }
  let placed = 0;
  const spots = [];
  for (let i = 0; i < W * H; i++) if (!safe.has(i)) spots.push(i);
  // 打乱（Fisher-Yates，用 xorshift）
  for (let i = spots.length - 1; i > 0; i--) {
    const j = Number(xorshift64() % BigInt(i + 1));
    const t = spots[i]; spots[i] = spots[j]; spots[j] = t;
  }
  for (let i = 0; i < MINES; i++) board.tiles[spots[i]].setBomb(true);
  return board;
}

let logs = [];
function log(s) { logs.push(s); }

// 真实揭示：调用后设置 value + is_covered；0 级联
function reveal(board, tile) {
  if (tile.isCovered() === false || tile.isBomb()) return;
  const adj = board.getAdjacent(tile);
  let v = 0;
  for (const a of adj) if (a.isBomb()) v++;
  tile.setValue(v);  // 同时 is_covered = false
  if (v === 0) {
    for (const a of adj) if (a.isCovered() && !a.isBomb()) reveal(board, a);
  }
}

function hiddenCount(board) {
  let n = 0;
  for (const t of board.tiles) if (t.isCovered()) n++;
  return n;
}
function revealedHidden(board) {
  const out = [];
  for (const t of board.tiles) if (!t.isCovered() && !t.isBomb()) out.push(t);
  return out;
}

function boardSummary(board) {
  let w = 0, f = 0, c = 0;
  for (const t of board.tiles) {
    if (t.isCovered()) c++;
    if (t.isSolverFoundBomb()) f++;
    if (!t.isCovered() && !t.isBomb()) w++;
  }
  return "hidden=" + c + " revealedNonMine=" + w + " solverKnownMines=" + f + " bombsLeft=" + (board.num_bombs - f);
}

// patch console.log to also capture solver verbose into currentTrace
const origConsoleLog = console.log;
let currentTrace = [];
console.log = (...args) => {
  const s = args.map(a => (typeof a === "string" ? a : String(a))).join(" ");
  currentTrace.push(s);
  origConsoleLog(...args);
};

async function playGame(seed) {
  board = makeBoard(seed);
  reveal(board, board.getTileXY(3, 3));
  currentTrace = [];

  let moves = 0;
  const MAX_MOVES = 400;
  let forcedGuessCount = 0;
  let maxBlended = 0;

  while (moves < MAX_MOVES) {
    moves++;
    const summary = boardSummary(board);
    currentTrace.push("===== move " + moves + " :: " + summary + " =====");
    const reply = await solver(board, { verbose: true, playStyle: 1, advancedGuessing: true });
    currentTrace.push("----- solver call end -----");

    const actions = reply.actions;
    if (!actions || actions.length === 0) {
      currentTrace.push("!! solver returned no actions");
      break;
    }

    // 判断是否猜测局（无 p=1 的 clear）
    let hasSafe = false;
    for (const a of actions) {
      if ((a.action === ACTION_CLEAR || a.action === ACTION_CHORD) && a.prob === 1 && !a.dead) { hasSafe = true; break; }
    }
    if (!hasSafe) forcedGuessCount++;

    let didAnything = false;
    for (const a of actions) {
      const t = board.getTileXY(a.x, a.y);
      if (a.action === ACTION_FLAG) {
        if (!t.isFlagged()) { t.setFoundBomb(); t.toggleFlag(); }
        didAnything = true;
      } else {
        if (t.isBomb()) {
          currentTrace.push("!! MOVE " + moves + " BLEW UP on " + t.asText() + " (prob " + a.prob + ")");
          fs.writeFileSync(path.join(__dirname, "ref_play_" + seed + ".txt"), currentTrace.join("\\n"), "utf8");
          return { seed: seed.toString(), result: "LOSE", moves, guesses: forcedGuessCount };
        }
        reveal(board, t);
        didAnything = true;
      }
    }

    // 赢 = 所有非雷格揭示（隐藏的剩余格全是雷）
    if (boardSummary(board).indexOf("revealedNonMine=" + (W * H - MINES)) !== -1) {
      fs.writeFileSync(path.join(__dirname, "ref_play_" + seed + ".txt"), currentTrace.join("\\n"), "utf8");
      return { seed: seed.toString(), result: "WIN", moves, guesses: forcedGuessCount };
    }
    if (!didAnything) { currentTrace.push("!! no action executed"); break; }
  }

  fs.writeFileSync(path.join(__dirname, "ref_play_" + seed + ".txt"), currentTrace.join("\\n"), "utf8");
  return { seed: seed.toString(), result: "STOP(" + boardSummary(board) + ")", moves, guesses: forcedGuessCount };
}

(async () => {
  const arg = process.argv[2];
  const seeds = arg ? [BigInt(arg)] : [BigInt(0xC0FFEE12345), BigInt(2), BigInt(3), BigInt(4), BigInt(5)];
  const results = [];
  for (const s of seeds) {
    const r = await playGame(s);
    results.push(r);
    console.log("RESULT " + JSON.stringify(r));
  }
  const wins = results.filter(r => r.result === "WIN").length;
  const totalMoves = results.reduce((a, r) => a + r.moves, 0);
  const totalGuesses = results.reduce((a, r) => a + r.guesses, 0);
  console.log("SUMMARY wins=" + wins + "/" + results.length + " totalMoves=" + totalMoves + " totalGuesses=" + totalGuesses);
})();
`;

fs.writeFileSync(path.join(__dirname, "headless_bundle.js"), code, "utf8");
console.log("bundle written: headless_bundle.js");