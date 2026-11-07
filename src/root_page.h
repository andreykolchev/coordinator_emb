#pragma once

constexpr char INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP32 Process Coordinator</title>

  <style>
    body {
      font-family: Arial, sans-serif;
      max-width: 760px;
      margin: 35px auto;
      padding: 0 16px;
      background: #f4f6f8;
      color: #202833;
    }

    .card {
      background: white;
      padding: 20px;
      margin: 16px 0;
      border-radius: 10px;
      box-shadow: 0 2px 8px #0001;
    }

    button {
      padding: 12px 18px;
      margin: 5px;
      cursor: pointer;
      border: 0;
      border-radius: 6px;
      background: #1769aa;
      color: white;
    }

    button:disabled {
      background: #aaa;
      cursor: not-allowed;
    }

    pre {
      white-space: pre-wrap;
      overflow-wrap: anywhere;
    }

    .status {
      font-weight: bold;
    }

    .worker {
      border-top: 1px solid #e3e7eb;
      padding: 12px 0;
    }

    .worker:first-of-type {
      border-top: 0;
    }

    .worker h3 {
      margin: 0 0 6px;
    }

    .worker p {
      margin: 4px 0;
    }

    .muted {
      color: #6b7682;
    }

    .busy {
      color: #b26a00;
    }

    .failed {
      color: #c62828;
    }

    select {
      padding: 10px;
      margin: 5px;
      border-radius: 6px;
    }

    #graph {
      display: block;
      width: 100%;
      margin: 10px auto;
    }

    #graph g.node {
      cursor: pointer;
    }

    #graph text {
      font-size: 12px;
      pointer-events: none;
    }
  </style>
</head>

<body>
  <h1>ESP32 Process Coordinator</h1>
  <p>Distributed execution demo</p>

  <div class="card">
    <h2>Network</h2>
    <p>Coordinator: <b id="host">-</b></p>
    <p>WebSocket:
      <span id="wsStatus" class="status">Connecting...</span>
    </p>
  </div>

  <div class="card">
    <h2>Process</h2>
    <button id="startBtn" disabled>Start</button>
    <button id="stopBtn" disabled>Stop</button>
    <p>Status: <span id="execStatus" class="status">no process</span></p>
    <svg id="graph" xmlns="http://www.w3.org/2000/svg"></svg>
    <pre id="nodeDetail" class="muted">Click a node to see its result</pre>
  </div>

  <div class="card">
    <h2>Workers</h2>
    <div id="workers"><p class="muted">No workers registered</p></div>
  </div>

  <div class="card">
    <h2>Execute a task</h2>
    <p class="muted">Runs on any idle worker with the capability.</p>
    <div id="tasks"><p class="muted">No capabilities available</p></div>
  </div>

  <div class="card">
    <h2>Process variables</h2>
    <pre id="variables">No variables yet</pre>
  </div>

  <div class="card">
    <h2>Event log</h2>
    <pre id="log"></pre>
  </div>

<script>
  let socket;

  function log(message) {
    const el = document.getElementById("log");
    el.textContent =
      new Date().toLocaleTimeString() + " " + message + "\n" +
      el.textContent;
  }

  document.getElementById("host").textContent = location.hostname;

  function renderVariables(variables) {
    document.getElementById("variables").textContent =
      variables && Object.keys(variables).length
        ? JSON.stringify(variables, null, 2)
        : "No variables yet";
  }

  function connect() {
    socket = new WebSocket("ws://" + location.hostname + ":81/");

    socket.onopen = () => {
      document.getElementById("wsStatus").textContent = "Connected";
      updateButtons();
      log("Dashboard connected");
    };

    socket.onclose = () => {
      document.getElementById("wsStatus").textContent = "Disconnected";
      updateButtons();
      log("Connection closed; retrying in 2 seconds");
      setTimeout(connect, 2000);
    };

    socket.onerror = () => {
      log("WebSocket error");
    };

    socket.onmessage = (event) => {
      const msg = JSON.parse(event.data);

      if (msg.type === "state") {
        renderWorkers(msg.workers || []);
        renderTasks(msg.workers || []);
      }

      if (msg.type === "variables") renderVariables(msg.variables);

      if (msg.type === "event") {
        log(msg.message);
      }

      if (msg.type === "snapshot") onSnapshot(msg);
      if (msg.type === "node_status") onNodeStatus(msg);
      if (msg.type === "execution_status") onExecutionStatus(msg);

      if (msg.type === "response" && !msg.ok) {
        for (const e of msg.errors) log("Request failed: " + e.code + ": " + e.message);
      }
    };
  }

  // Worker data comes from the network, so build nodes with
  // textContent rather than innerHTML.
  function el(tag, text, className) {
    const node = document.createElement(tag);

    if (text !== undefined) node.textContent = text;
    if (className) node.className = className;

    return node;
  }

  function field(label, value, className) {
    const p = el("p", label + ": ");
    p.appendChild(el("span", value, className));
    return p;
  }

  function taskButton(name, workerId, disabled) {
    const button = el("button", name.replace(/_/g, " "));
    button.disabled = disabled;
    button.onclick = () => runTask(name, workerId);
    return button;
  }

  function renderWorkers(workers) {
    const container = document.getElementById("workers");
    container.replaceChildren();

    if (!workers.length) {
      container.appendChild(el("p", "No workers registered", "muted"));
      return;
    }

    for (const w of workers) {
      const statusClass = w.busy ? "status busy"
        : w.status.startsWith("Failed") ? "status failed" : "status";

      const item = el("div", undefined, "worker");
      item.appendChild(el("h3", w.id));
      item.appendChild(field("IP", w.ip || "-"));
      item.appendChild(field("Status", w.status, statusClass));
      item.appendChild(field("Last task", w.task_id
        ? w.task_id + " (" + w.task_name + ")" : "-"));
      item.appendChild(field("Result", w.result));

      const buttons = el("div");

      for (const cap of w.capabilities) {
        buttons.appendChild(taskButton(cap, w.id, w.busy));
      }

      if (!w.capabilities.length) {
        buttons.appendChild(el("p", "No capabilities", "muted"));
      }

      item.appendChild(buttons);
      container.appendChild(item);
    }
  }

  // One button per capability offered by any worker; disabled while
  // every worker offering it is busy.
  function renderTasks(workers) {
    const container = document.getElementById("tasks");
    container.replaceChildren();

    const idle = new Map();

    for (const w of workers) {
      for (const cap of w.capabilities) {
        idle.set(cap, (idle.get(cap) || false) || !w.busy);
      }
    }

    if (!idle.size) {
      container.appendChild(el("p", "No capabilities available", "muted"));
      return;
    }

    for (const [cap, anyIdle] of idle) {
      container.appendChild(taskButton(cap, "", !anyIdle));
    }
  }

  function runTask(name, workerId) {
    if (!socket || socket.readyState !== WebSocket.OPEN) {
      log("Dashboard is not connected");
      return;
    }

    const message = { type: "run_task", name: name };

    if (workerId) message.worker_id = workerId;

    socket.send(JSON.stringify(message));

    log("Requested task: " + name + (workerId ? " on " + workerId : ""));
  }


  // ---- Process ----
  // Everything shown here comes from coordinator messages: "snapshot" on
  // connect, then "node_status" and "execution_status" as things change.
  // The page never guesses what the coordinator will do next.

  // The process is in the engine's own format: nodes of type Start, Stop,
  // Router, Fork, Join, ExternalTask and InternalTask, linked by routes {from, to}.
  let definition = null;      // the process JSON {version, process: {...}}
  let execution = null;       // {id, status, reason, nodes: {node_id: status}}
  let selectedNodeId = null;  // node shown in the detail box
  let requestCounter = 0;

  const STATE_COLORS = {
    pending: "#cfd6dd", ready: "#f0c24b", running: "#1769aa",
    succeeded: "#2e9e5b", failed: "#c62828", cancelled: "#8a8f98"
  };
  // States drawn on a dark color need white text.
  const WHITE_TEXT_STATES = new Set(["running", "succeeded", "failed", "cancelled"]);

  // Send a command; the coordinator answers with a "response".
  function sendCommand(message) {
    if (!socket || socket.readyState !== WebSocket.OPEN) {
      log("Dashboard is not connected");
      return;
    }
    message.request_id = "r" + (++requestCounter);
    socket.send(JSON.stringify(message));
  }

  function isRunning() {
    return !!execution && execution.status === "running";
  }

  function updateButtons() {
    const connected = !!socket && socket.readyState === WebSocket.OPEN;
    document.getElementById("startBtn").disabled =
      !connected || isRunning() || !definition;
    document.getElementById("stopBtn").disabled = !connected || !isRunning();
  }

  // ---- Coordinator messages ----

  // Full state: replaces whatever the page showed before.
  function onSnapshot(msg) {
    definition = msg.process;
    renderVariables(msg.variables);
    execution = null;
    selectedNodeId = null;
    if (msg.execution) {
      execution = { id: msg.execution.id, status: msg.execution.status,
                    reason: msg.execution.reason, nodes: {} };
      for (const node of msg.execution.nodes) {
        execution.nodes[node.node_id] = node;
      }
    }
    renderProcess();
  }

  // The first message about a new execution ID starts a fresh one.
  function executionFor(id) {
    if (!execution || execution.id !== id) {
      execution = { id: id, status: "running", reason: "", nodes: {} };
    }
    return execution;
  }

  function onNodeStatus(msg) {
    executionFor(msg.execution_id).nodes[msg.node_id] = msg;
    renderProcess();
  }

  function onExecutionStatus(msg) {
    const current = executionFor(msg.execution_id);
    current.status = msg.status;
    current.reason = msg.reason || "";
    renderProcess();
  }

  // A Join counts as arrived for each incoming route whose node has finished
  // and not moved on: the coordinator only reports the Join itself when it fires.
  function joinArrivals(nodeId) {
    const incoming = definition.process.routes.filter((c) => c.to === nodeId);
    const arrived = incoming.filter((c) => {
      const from = execution.nodes[c.from];
      return from && from.state === "succeeded" && !from.next;
    }).length;
    return { arrived: arrived, incoming: incoming.length };
  }

  function nodeState(nodeId) {
    const status = execution && execution.nodes[nodeId];
    const node = definition && definition.process.nodes.find((n) => n.id === nodeId);
    if (node && node.type === "Join" && isRunning() && joinArrivals(nodeId).arrived > 0) {
      return "ready";  // some branches are waiting here for the others
    }
    if (!status) return "pending";
    // Green is only for an execution in progress; once it has ended, nodes
    // that succeeded go back to pending. Failed and cancelled stay visible.
    if (status.state === "succeeded" && !isRunning()) return "pending";
    return status.state;
  }

  // How many times a loop has run the node (0 until it runs more than once).
  function nodeRuns(nodeId) {
    const status = execution && execution.nodes[nodeId];
    return status && status.runs > 1 && nodeState(nodeId) !== "pending" ? status.runs : 0;
  }

  // Text shown on a node: its state, or just the run count once it has looped
  // (the color already tells the state).
  function stateLabel(nodeId) {
    if (nodeState(nodeId) === "ready") {
      const join = joinArrivals(nodeId);
      return "waiting " + join.arrived + "/" + join.incoming;
    }
    return nodeRuns(nodeId) || nodeState(nodeId);
  }

  // ---- Drawing ----

  function renderProcess() {
    const status = document.getElementById("execStatus");
    if (!definition) {
      status.textContent = "no process";
    } else if (!execution) {
      status.textContent = definition.process.id + ": ready to start";
    } else {
      status.textContent = definition.process.id + ": " + execution.status +
        (execution.reason ? " (" + execution.reason + ")" : "") +
        " [" + execution.id + "]";
    }
    renderGraph();
    renderNodeDetail();
    updateButtons();
  }

  function renderNodeDetail() {
    const box = document.getElementById("nodeDetail");
    const node = definition && definition.process.nodes.find((n) => n.id === selectedNodeId);
    if (!node) {
      box.textContent = "Click a node to see its result";
      return;
    }
    const status = (execution && execution.nodes[node.id]) || null;
    box.textContent = JSON.stringify({ node: node, status: status }, null, 2);
  }

  // Connections that lead back to a node still being visited from the start
  // (loops). They are drawn separately and ignored when ranking.
  function backEdges(nodes, connections) {
    const out = new Map(nodes.map((node) => [node.id, []]));
    for (const c of connections) if (out.has(c.from)) out.get(c.from).push(c);
    const back = new Set(), state = new Map();
    const visit = (id) => {
      state.set(id, 1);
      for (const c of out.get(id) || []) {
        if (!out.has(c.to)) continue;
        if (state.get(c.to) === 1) back.add(c);
        else if (!state.has(c.to)) visit(c.to);
      }
      state.set(id, 2);
    };
    for (const node of nodes) if (!state.has(node.id)) visit(node.id);
    return back;
  }

  // Give each node a row (rank) and a place in that row (slot).
  // rank = length of the longest path leading to the node, so every arrow
  // points down. A chain gets one node per row; nodes on parallel branches
  // (after a Fork) share a row.
  function layout(nodes, connections) {
    const indexById = new Map(nodes.map((node, i) => [node.id, i]));
    const rank = nodes.map(() => 0);
    const back = backEdges(nodes, connections);
    // Repeat at most nodes.length times so a cycle cannot loop forever.
    for (let pass = 0; pass < nodes.length; pass++) {
      for (const c of connections) {
        const from = indexById.get(c.from), to = indexById.get(c.to);
        if (back.has(c)) continue; // a loop-back arrow does not move nodes
        if (from !== undefined && to !== undefined && rank[to] < rank[from] + 1) {
          rank[to] = rank[from] + 1;
        }
      }
    }
    const nodesInRank = [];
    const positions = nodes.map((node, i) => {
      nodesInRank[rank[i]] = (nodesInRank[rank[i]] || 0) + 1;
      return { rank: rank[i], slot: nodesInRank[rank[i]] - 1 };
    });
    for (const p of positions) p.rankSize = nodesInRank[p.rank];
    return positions;
  }

  // Create an SVG element. Text goes in with textContent, never as HTML.
  function svg(tag, attributes, text) {
    const element = document.createElementNS("http://www.w3.org/2000/svg", tag);
    for (const name in attributes) element.setAttribute(name, attributes[name]);
    if (text !== undefined) element.textContent = text;
    return element;
  }

  function shorten(text, maxLength) {
    text = String(text);
    return text.length > maxLength ? text.slice(0, maxLength - 1) + "…" : text;
  }

  // One line describing what a task does.
  // Text on a connection: the test a gateway applies, or "else".
  function describeRoute(c, fromNode) {
    if (c.condition) {
      return c.condition.variable + " " + c.condition.op + " " + JSON.stringify(c.condition.value);
    }
    return fromNode.type === "Router" ? "else" : "";
  }

  // Defaults as in the engine (DEFAULT_TASK_TIMEOUT_MS).
  function describeTask(node) {
    if (node.type === "ExternalTask") {
      return node.externalTaskType + (node.resultVariable ? " → " + node.resultVariable : "");
    }
    if (node.type === "InternalTask") {
      switch (node.internalTaskType) {
        case "Delay": return "delay " + (node.delayMs ?? 5000) + " ms";
        case "Increment": return "increment " + node.variable + " by " + (node.increment ?? 1);
        case "SetVariable": return "set " + node.variable + " = " + JSON.stringify(node.value ?? null);
      }
    }
    return "";
  }

  function renderGraph() {
    const root = document.getElementById("graph");
    root.replaceChildren();
    if (!definition) return;

    const nodes = definition.process.nodes || [];
    const connections = definition.process.routes || [];
    const positions = layout(nodes, connections);
    const indexById = new Map(nodes.map((node, i) => [node.id, i]));

    const BOX_WIDTH = 230, GAP_X = 20, GAP_Y = 36, PADDING = 12;
    const GATEWAY_HALF_WIDTH = 70;
    // Events are small circles, tasks are taller boxes.
    const isEvent = (node) => node.type === "Start" || node.type === "Stop";
    // Fork and Join are bars across the nodes they connect, as in a UML activity
    // diagram; arrows leave and enter the bar straight above or below each node.
    const isBar = (node) => node.type === "Fork" || node.type === "Join";
    const halfHeight = (node) => (isEvent(node) ? 16 : isBar(node) ? 5 : 24);
    // How far a node reaches to the right of its center.
    const halfWidth = (i) => isEvent(nodes[i]) ? 14
      : nodes[i].type === "Router" ? GATEWAY_HALF_WIDTH
      : isBar(nodes[i]) ? barSpan(i)[1] - centerX(i) : BOX_WIDTH / 2;

    // Canvas width fits the widest row.
    const widestRow = Math.max(1, ...positions.map((p) => p.rankSize));
    const contentWidth = widestRow * (BOX_WIDTH + GAP_X) - GAP_X + 2 * PADDING;
    // Loop-back arrows run down the right side, so leave room for them.
    const back = backEdges(nodes, connections);
    const width = contentWidth + (back.size ? 30 : 0);

    // Each row is as tall as its tallest node; rowCenterY is its middle.
    const rowHeight = [];
    nodes.forEach((node, i) => {
      const r = positions[i].rank;
      rowHeight[r] = Math.max(rowHeight[r] || 0, 2 * halfHeight(node));
    });
    const rowCenterY = [];
    let y = PADDING;
    for (let r = 0; r < rowHeight.length; r++) {
      rowCenterY[r] = y + rowHeight[r] / 2;
      y += rowHeight[r] + GAP_Y;
    }
    const height = y - GAP_Y + PADDING;
    root.setAttribute("viewBox", "0 0 " + width + " " + height);
    root.style.maxWidth = width + "px";   // never scale up beyond natural size

    // Center of node i: rows are centered horizontally.
    const centerX = (i) => contentWidth / 2 +
      (positions[i].slot - (positions[i].rankSize - 1) / 2) * (BOX_WIDTH + GAP_X);
    const centerY = (i) => rowCenterY[positions[i].rank];
    // A bar reaches across the boxes it forks into or joins from.
    const barSpan = (i) => {
      const fork = nodes[i].type === "Fork";
      let left = centerX(i) - 20, right = centerX(i) + 20;
      for (const c of connections) {
        const other = indexById.get(fork ? c.to : c.from);
        if (back.has(c) || (fork ? c.from : c.to) !== nodes[i].id || other === undefined) continue;
        left = Math.min(left, centerX(other) - BOX_WIDTH / 2);
        right = Math.max(right, centerX(other) + BOX_WIDTH / 2);
      }
      return [left, right];
    };

    // Arrowhead used by every connection line.
    const defs = svg("defs", {});
    const marker = svg("marker", { id: "arrow", markerWidth: 8, markerHeight: 8,
      refX: 7, refY: 4, orient: "auto" });
    marker.appendChild(svg("path", { d: "M0,0 L8,4 L0,8 z", fill: "#6b7682" }));
    defs.appendChild(marker);
    const takenMarker = svg("marker", { id: "arrowTaken", markerWidth: 8, markerHeight: 8,
      refX: 7, refY: 4, orient: "auto" });
    takenMarker.appendChild(svg("path", { d: "M0,0 L8,4 L0,8 z", fill: STATE_COLORS.succeeded }));
    defs.appendChild(takenMarker);
    root.appendChild(defs);

    // Connections: from the bottom of one node to the top of the next.
    for (const c of connections) {
      const from = indexById.get(c.from), to = indexById.get(c.to);
      if (from === undefined || to === undefined) continue;
      // Green once the execution has moved along this connection.
      const fromStatus = execution && execution.nodes[c.from];
      const succeeded = isRunning() && !!fromStatus && fromStatus.state === "succeeded";
      // A Fork takes all its routes at once, but reports one "next": its other targets
      // count as taken once they have started. A branch that reached a Join and waits
      // there has not moved on yet either.
      const taken = succeeded && (fromStatus.next === c.to ||
        (nodes[from].type === "Fork" && !!execution.nodes[c.to]) ||
        (nodes[to].type === "Join" && !fromStatus.next));
      const color = taken ? STATE_COLORS.succeeded : "#6b7682";
      const arrow = taken ? "url(#arrowTaken)" : "url(#arrow)";
      if (back.has(c)) {
        // Out of the right side of the later node, up, into the earlier one.
        const edge = centerX(from) + halfWidth(from);
        const outX = contentWidth - PADDING + 16; // right of the widest row, clears every box
        root.appendChild(svg("path", {
          d: "M" + edge + "," + centerY(from) + " H" + outX + " V" + centerY(to) +
             " H" + (centerX(to) + halfWidth(to) + 2),
          fill: "none", stroke: color, "stroke-width": 1.5,
          "stroke-dasharray": "5 3", "marker-end": arrow
        }));
        const backLabel = describeRoute(c, nodes[from]);
        if (backLabel) {
          root.appendChild(svg("text", { x: edge + 4, y: centerY(from) - 4,
            fill: "#4a5560", "font-size": 11 }, backLabel));
        }
        continue;
      }
      const down = centerY(to) >= centerY(from);
      // To or from a bar the arrow runs straight above or below the node at its other end.
      const x1 = isBar(nodes[from]) && !isBar(nodes[to]) ? centerX(to) : centerX(from);
      const x2 = isBar(nodes[to]) && !isBar(nodes[from]) ? centerX(from) : centerX(to);
      root.appendChild(svg("line", {
        x1: x1,
        y1: centerY(from) + (down ? halfHeight(nodes[from]) : -halfHeight(nodes[from])),
        x2: x2,
        y2: centerY(to) + (down ? -halfHeight(nodes[to]) - 2 : halfHeight(nodes[to]) + 2),
        stroke: color, "stroke-width": taken ? 2 : 1.5, "marker-end": arrow
      }));
      const label = describeRoute(c, nodes[from]);
      if (label) {
        root.appendChild(svg("text", { x: (x1 + x2) / 2 + 8,
          y: (centerY(from) + centerY(to)) / 2 + 4, fill: "#4a5560", "font-size": 11 }, label));
      }
    }

    // Nodes, colored by their state.
    nodes.forEach((node, i) => {
      const state = nodeState(node.id);
      const fill = STATE_COLORS[state] || STATE_COLORS.pending;
      const textColor = WHITE_TEXT_STATES.has(state) ? "#fff" : "#202833";
      const isSelected = node.id === selectedNodeId;
      const outline = isSelected ? "#202833" : "#6b7682";
      const x = centerX(i), y = centerY(i), left = x - BOX_WIDTH / 2;

      const group = svg("g", { class: "node" });
      group.onclick = () => { selectedNodeId = node.id; renderProcess(); };
      const stateText = stateLabel(node.id);
      group.appendChild(svg("title", {}, node.id + " (" + state +
        (nodeRuns(node.id) ? ", run " + nodeRuns(node.id) : "") + ")"));

      if (isEvent(node)) {
        // Start: plain circle. Stop: circle with a thick ring. The circle sits
        // on the center line, where the arrows are; the label is beside it.
        const isStop = node.type === "Stop";
        group.appendChild(svg("circle", { cx: x, cy: y, r: 14, fill: fill,
          stroke: outline, "stroke-width": isStop ? 4 : 1.5 }));
        group.appendChild(svg("text", { x: x + 22, y: y + 4, fill: "#202833" },
          shorten(node.id, 24) + " · " + stateText));
      } else if (isBar(node)) {
        // Fork: label above the bar, Join: below it, both at the right end.
        const [barLeft, barRight] = barSpan(i);
        group.appendChild(svg("rect", { x: barLeft, y: y - 5, width: barRight - barLeft, height: 10,
          rx: 2, fill: fill, stroke: outline, "stroke-width": isSelected ? 3 : 1.5 }));
        group.appendChild(svg("text", { x: barRight, y: node.type === "Fork" ? y - 11 : y + 21,
          fill: "#202833", "text-anchor": "end" }, shorten(node.id, 24) + " · " + stateText));
      } else if (node.type === "Router") {
        // Decision: diamond. Which way it went shows on the arrows.
        const w = GATEWAY_HALF_WIDTH;
        group.appendChild(svg("polygon", {
          points: (x - w) + "," + y + " " + x + "," + (y - 24) + " " +
                  (x + w) + "," + y + " " + x + "," + (y + 24),
          fill: fill, stroke: outline, "stroke-width": isSelected ? 3 : 1.5 }));
        group.appendChild(svg("text", { x: x, y: y - 1, fill: textColor,
          "font-weight": "bold", "text-anchor": "middle" }, shorten(node.id, 12)));
        group.appendChild(svg("text", { x: x, y: y + 12, fill: textColor,
          "font-size": 10, "text-anchor": "middle" }, shorten(stateText, 16)));
      } else {
        // Internal task: rounded box. External task: square box marked ⇄.
        const isExternal = node.type === "ExternalTask";
        group.appendChild(svg("rect", { x: left, y: y - 24, width: BOX_WIDTH, height: 48,
          rx: isExternal ? 3 : 14, fill: fill,
          stroke: outline, "stroke-width": isSelected ? 3 : 1.5 }));
        const title = (isExternal ? "⇄ " : "") + shorten(node.id, 18);
        group.appendChild(svg("text", { x: left + 10, y: y - 4, fill: textColor,
          "font-weight": "bold" }, title));
        group.appendChild(svg("text", { x: left + BOX_WIDTH - 10, y: y - 4, fill: textColor,
          "font-size": 11, "text-anchor": "end" }, stateText));
        group.appendChild(svg("text", { x: left + 10, y: y + 13, fill: textColor,
          "font-size": 11 }, shorten(describeTask(node), 34)));
      }
      root.appendChild(group);
    });
  }

  document.getElementById("startBtn").onclick = () =>
    sendCommand({ type: "start_process" });
  document.getElementById("stopBtn").onclick = () =>
    sendCommand({ type: "stop_process" });

  connect();
</script>
</body>
</html>
)rawliteral";
