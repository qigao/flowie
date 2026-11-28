"use strict";

const element = (id) => document.getElementById(id);
const domainPath = new URL("./", window.location.href);
const releasePath = new URL("releases/", domainPath);
const state = {
  token: null,
  context: null,
  busy: false,
  activeDigest: null,
  digest: null,
  preview: false,
  next: null,
  validation: null,
  names: new Map(),
};

const messages = {
  release_credentials_required: "Enter a valid Flowie Control management session token.",
  release_access_denied: "This account lacks permission for this Domain or operation.",
  release_authority_unavailable: "Flowie Control is temporarily unavailable.",
  package_name_required: "Add a package name to the manifest and sign the package before uploading.",
  release_trust_rejected: "The package signature, signer, or Domain scope was rejected.",
  release_content_rejected: "The package contents failed validation.",
  rulesforge_runtime_required: "This package requires a RulesForge runtime for the domain.",
  release_base_conflict: "The package was built from a different active release.",
  validation_required: "Revalidate the package against the current release.",
  stale_precondition: "The active release changed. Refresh and validate again.",
  release_limit_exceeded: "The archive or repository exceeds its configured limit.",
  invalid_acl_text: "The signed ACL cannot be displayed as a text document.",
  release_not_found: "The selected package is unavailable.",
  idempotency_conflict: "The activation key has already been used for a different request.",
  release_unavailable: "The release service is temporarily unavailable.",
};

function feedback(message, error = false) {
  const status = element("feedback");
  status.textContent = message;
  status.dataset.error = String(error);
}

function updateControls() {
  element("upload").disabled = state.busy || !state.context;
  element("refresh").disabled = state.busy;
  element("validate").disabled = state.busy || !state.preview || state.digest === state.activeDigest;
  element("activate").disabled = state.busy || !state.validation;
  element("more").hidden = !state.next;
  element("more").disabled = state.busy;
  for (const button of element("history").querySelectorAll("button")) {
    button.setAttribute("aria-current", String(button.dataset.digest === state.digest));
    button.setAttribute("aria-disabled", String(state.busy));
    const name = state.names.get(button.dataset.digest);
    button.firstElementChild.textContent = name || button.dataset.digest.slice(0, 12);
    button.setAttribute("aria-label", "Preview " + (name || "package") + " " + button.dataset.digest);
  }
}

async function operate(message, task) {
  if (state.busy) return;
  state.busy = true;
  updateControls();
  feedback(message);
  try {
    feedback(await task());
  } catch (error) {
    feedback(error.message, true);
  } finally {
    state.busy = false;
    updateControls();
  }
}

async function request(path, options = {}) {
  const headers = new Headers(options.headers);
  if (state.token) headers.set("Authorization", "Bearer " + state.token);
  return fetch(path, {
    ...options,
    headers,
    mode: "same-origin",
    cache: "no-store",
    redirect: "error",
    signal: AbortSignal.timeout(45000),
  });
}

async function requireSuccess(response) {
  if (response.ok) return response;
  let code;
  if (response.headers.get("Content-Type")?.includes("application/json")) {
    code = (await response.json()).code;
  }
  if (response.status === 401) {
    state.token = null;
    state.context = null;
    state.preview = false;
    state.digest = null;
    state.validation = null;
    state.next = null;
    element("history").replaceChildren();
    element("candidate").hidden = true;
    element("acl-text").textContent = "";
    element("certificates").hidden = true;
    renderActive(null, null);
    element("management-session").hidden = false;
    element("management-token").focus();
  }
  const error = new Error(messages[code] || "The request failed (HTTP " + response.status + ").");
  error.status = response.status;
  throw error;
}

async function activeRelease() {
  const response = await request(new URL("active-release", domainPath));
  if (response.status === 404 && (await response.clone().json()).code === "no_active_release") return null;
  await requireSuccess(response);
  const active = await response.json();
  const etag = response.headers.get("ETag");
  const generation = etag?.match(/^"release-([1-9][0-9]*)"$/);
  if (!generation) throw new Error("The active release has no valid generation tag.");
  return {digest: active.digest, aclVersion: active.acl_version, generation: generation[1], etag};
}

function renderActive(active, ready) {
  state.activeDigest = active?.digest ?? null;
  element("generation").textContent = active?.generation ?? "—";
  element("acl-version").textContent = active?.aclVersion ?? "—";
  element("active-digest").textContent = active?.digest ?? "No active release";
  const runtime = element("runtime");
  runtime.dataset.ready = String(ready === true);
  runtime.textContent = ready === true ? "Gateway ACL ready"
    : ready === false ? "Gateway ACL unavailable" : "Gateway ACL readiness unknown";
}

async function updateActive() {
  const active = await activeRelease();
  renderActive(active, null);
  const response = await requireSuccess(await request(new URL("status", domainPath)));
  const status = await response.json();
  const ready = active && Number(active.generation) === status.expected_generation
    ? status.ready : active ? null : false;
  renderActive(active, ready);
  return active;
}

function historyItem(entry) {
  const item = document.createElement("li");
  const button = document.createElement("button");
  button.type = "button";
  button.className = "secondary history-item";
  button.dataset.digest = entry.digest;
  const digest = document.createElement("span");
  digest.className = "mono";
  digest.textContent = entry.digest.slice(0, 12);
  const date = new Date(entry.admitted_at * 1000);
  const time = document.createElement("time");
  time.dateTime = date.toISOString();
  time.textContent = date.toLocaleString();
  button.append(digest, time);
  button.addEventListener("click", () => operate("Reading signed ACL…", async () => {
    await selectPackage(entry.digest);
    return "Signed ACL loaded.";
  }));
  item.append(button);
  return item;
}

async function loadHistory(after = null) {
  const url = new URL("releases", domainPath);
  if (after) url.searchParams.set("after", after);
  const response = await requireSuccess(await request(url));
  const page = await response.json();
  const items = page.releases.map(historyItem);
  if (after) element("history").append(...items);
  else element("history").replaceChildren(...items);
  state.next = page.next_after;
  const empty = element("empty-history");
  empty.textContent = "No packages uploaded.";
  empty.hidden = element("history").childElementCount > 0;
}

function clearValidation() {
  state.validation = null;
  element("validation").textContent = state.digest && state.digest === state.activeDigest
    ? "Active package" : "Validate this package before activation.";
}

async function selectPackage(digest) {
  state.digest = digest;
  state.preview = false;
  clearValidation();
  element("candidate").hidden = true;
  element("selection-hint").hidden = false;
  element("selection-hint").textContent = "Reading signed ACL…";
  try {
    const response = await requireSuccess(await request(new URL(digest + "/acl", releasePath)));
    const name = response.headers.get("Flowie-Package-Name");
    const acl = await response.text();
    if (name) state.names.set(digest, name);
    element("package-name").textContent = name || "Unnamed package";
    element("package-digest").textContent = digest;
    element("acl-path").textContent = response.headers.get("Flowie-ACL-Path") || "Signed ACL";
    element("acl-text").textContent = acl;
    const download = element("download");
    download.href = new URL(digest, releasePath);
    download.download = (name ? name + "-" : "") + digest.slice(0, 12) + ".tar.zst";
    element("candidate").hidden = false;
    element("selection-hint").hidden = true;
    state.preview = true;
  } catch (error) {
    element("selection-hint").textContent = "Select a package to preview its signed ACL.";
    throw error;
  }
}

async function refresh() {
  if (!state.context) {
    const response = await requireSuccess(await request(new URL("packages/context", domainPath)));
    const signer = response.headers.get("Flowie-Release-Signer");
    if (!signer) throw new Error("The Domain has no configured release signer.");
    state.context = {...await response.json(), signer};
    element("scope").textContent = state.context.domain + " · " + state.context.environment;
    element("operator-id").textContent = state.context.operator_id;
    element("operator-fingerprint").textContent = state.context.operator_certificate_sha256;
    element("signer-reference").textContent = state.context.signer_reference;
    element("signer-fingerprint").textContent = state.context.signer_certificate_sha256;
    element("certificates").hidden = false;
  }
  const active = await updateActive();
  await loadHistory();
  if (state.digest || active) await selectPackage(state.digest || active.digest);
  return "";
}

element("refresh").addEventListener("click", () => operate("Refreshing packages…", refresh));

element("management-session").addEventListener("submit", (event) => {
  event.preventDefault();
  if (state.busy) return;
  const input = element("management-token");
  state.token = input.value.trim();
  input.value = "";
  state.context = null;
  operate("Checking the Control session…", refresh);
});

element("download").addEventListener("click", (event) => {
  if (!state.token) return;
  event.preventDefault();
  operate("Downloading the signed package…", async () => {
    const link = event.currentTarget;
    const response = await requireSuccess(await request(new URL(link.href)));
    const url = URL.createObjectURL(await response.blob());
    const download = document.createElement("a");
    download.href = url;
    download.download = link.download;
    download.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
    return "Package downloaded.";
  });
});

element("more").addEventListener("click", () => operate("Loading package history…", async () => {
  await loadHistory(state.next);
  return "";
}));

element("upload-form").addEventListener("submit", (event) => {
  event.preventDefault();
  operate("Uploading and verifying the signed package…", async () => {
    const input = element("archive");
    const response = await requireSuccess(await request(new URL("releases", domainPath), {
      method: "POST",
      headers: {"Content-Type": "application/zstd", "Flowie-Release-Signer": state.context.signer},
      body: input.files[0],
    }));
    const receipt = await response.json();
    input.value = "";
    await selectPackage(receipt.digest);
    await loadHistory();
    element("candidate-heading").focus();
    return response.status === 201 ? "Signed package uploaded." : "This package is already stored.";
  });
});

element("validate").addEventListener("click", () => operate("Validating the signed package…", async () => {
  clearValidation();
  const active = await activeRelease();
  const response = await requireSuccess(await request(new URL(state.digest + "/validate", releasePath), {
    method: "POST",
  }));
  const result = await response.json();
  if (result.base_generation !== (active ? Number(active.generation) : 0)) {
    throw new Error("The active release changed during validation. Validate again.");
  }
  state.validation = {etag: active?.etag, key: crypto.randomUUID()};
  element("validation").textContent = result.acl_changed
    ? "Validated · the ACL will change." : "Validated · the ACL is unchanged.";
  return "Package validated.";
}));

element("activate").addEventListener("click", () => operate("Activating the validated ACL…", async () => {
  const attempt = state.validation;
  const headers = {"Content-Type": "application/json", "Idempotency-Key": attempt.key};
  if (attempt.etag) headers["If-Match"] = attempt.etag;
  else headers["If-None-Match"] = "*";

  let response;
  try {
    response = await requireSuccess(await request(new URL("active-release", domainPath), {
      method: "PUT", headers, body: JSON.stringify({digest: state.digest}),
    }));
  } catch (error) {
    if ([409, 412, 422].includes(error.status)) clearValidation();
    if (!error.status) throw new Error("Activation outcome is unknown. Use Activate package again to check the same attempt.");
    throw error;
  }
  clearValidation();
  const ready = response.headers.get("Flowie-Runtime-Ready");
  element("validation").textContent = "Package activation committed.";
  try {
    await updateActive();
    clearValidation();
  } catch {
    return "Activation committed. Refresh to check runtime readiness.";
  }
  return ready === "true" ? "Package activated. Gateway ACL ready." : "Activation committed. Check the gateway ACL status.";
}));

operate("Loading signed package management…", refresh);
