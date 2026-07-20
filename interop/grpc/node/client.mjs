// bridge_client_node: the payment walk, driven from Node against the bridge
// server, built from nothing but ashc's emitted artifacts. The .proto rides
// in through @grpc/proto-loader, so there is no protoc step and no generated
// stub; the session rides in through the emitted TypeScript wrapper, which
// node runs directly under type stripping. This is the editor extension's
// exact diet: two npm packages, no native code, one emitted file per role.
//
// The walk asserts what the Go client asserts: the vow override and the
// pinned shape hash at sign, the pending order, the value Err as err=41 on
// an OK rpc, the automatic break keeping its payload and the explicit break
// zeroing it, a lying hash refused as ABORTED, a killed client's instance
// collected in milliseconds, and a session quiet for three times the old
// timer resuming on latches set before the silence.

import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { setTimeout as sleep } from "node:timers/promises";

import grpc from "@grpc/grpc-js";
import protoLoader from "@grpc/proto-loader";

import {
  PaymentServiceShapeHash,
  openPaymentServiceSession,
} from "./gen/payment_session.ts";

const addr = process.argv.includes("--addr")
  ? process.argv[process.argv.indexOf("--addr") + 1]
  : "127.0.0.1:50254";
const legacyTtlMs = 2000;

let failures = 0;

function check(cond, what) {
  if (!cond) {
    console.error(`[bridge_client_node] FAIL: ${what}`);
    failures = 1;
  }
}

function makeClient() {
  const def = protoLoader.loadSync(
    fileURLToPath(new URL("./gen/payment.proto", import.meta.url)),
    { keepCase: true, longs: String, enums: String, oneofs: true, defaults: false },
  );
  const pkg = grpc.loadPackageDefinition(def);
  return new pkg.ashford.payment.PaymentService(
    addr,
    grpc.credentials.createInsecure(),
  );
}

// The unary surface as promises. A pledge's Err never rejects: only a
// fulfillment that did not run does.
function call(client, method, req) {
  return new Promise((resolve, reject) => {
    client[method](req, (err, reply) => {
      if (err) {
        reject(err);
      } else {
        resolve(reply);
      }
    });
  });
}

async function expectCode(promise, code, name, what) {
  try {
    await promise;
    check(false, `${what} did not fail`);
  } catch (err) {
    check(err.code === code, `${what} is ${name}, got code ${err.code}`);
  }
}

function checkOk(reply, what) {
  check(reply.result === "ok" && reply.ok === true, what);
}

function sliceEq(got, want) {
  return (
    got.length === want.length && got.every((v, i) => v === want[i])
  );
}

// Polls an instance id until it answers NOT_FOUND, and answers how long
// that took, or -1 on timeout. GetPartial reads without touching the
// instance, so the poll cannot change what it measures.
async function waitGone(client, id, timeoutMs) {
  const t0 = Date.now();
  while (Date.now() - t0 < timeoutMs) {
    try {
      await call(client, "GetPartial", { instance_id: id });
    } catch (err) {
      if (err.code === grpc.status.NOT_FOUND) {
        return Date.now() - t0;
      }
      throw err;
    }
    await sleep(5);
  }
  return -1;
}

async function walk(client) {
  const pinned = PaymentServiceShapeHash.toString();

  // ---- the partial path, signed with a vow override and a pinned hash ----

  const s1 = await openPaymentServiceSession(client, {
    currency: "EUR",
    expected_hash: pinned,
  });
  const c1 = s1.signed.instance_id;
  check(c1 !== "0", "the session stream issued an instance id");
  check(s1.signed.currency === "EUR", "vow override landed across the wire");
  check(
    s1.signed.shape_hash === pinned,
    "the signature carries the hash the wrapper pinned",
  );
  console.log(
    `[bridge_client_node] session ${c1} open, currency ${s1.signed.currency}`,
  );

  let p = await call(client, "GetPartial", { instance_id: c1 });
  check(
    sliceEq(p.pending, ["Validation", "Processing", "notify_user"]),
    "c1 pending order is subs then loose pledges",
  );

  checkOk(
    await call(client, "ValidateCard", { instance_id: c1, card: "4111 1111" }),
    "validate_card Ok",
  );
  p = await call(client, "GetPartial", { instance_id: c1 });
  check(p.state === "SIGNED", "half a subcontract moves nothing");

  checkOk(
    await call(client, "ValidateAmount", { instance_id: c1, amount: 25.0 }),
    "validate_amount Ok",
  );
  p = await call(client, "GetPartial", { instance_id: c1 });
  check(p.state === "PARTIAL", "Validation lands, c1 partial");
  check(
    sliceEq(p.fulfilled, ["Validation"]) &&
      sliceEq(p.pending, ["Processing", "notify_user"]) &&
      (p.broken ?? []).length === 0 &&
      (p.errors ?? []).length === 0,
    "partial names after Validation",
  );
  console.log(
    `[bridge_client_node] state PARTIAL, fulfilled ${p.fulfilled}, pending ${p.pending}`,
  );

  checkOk(
    await call(client, "Charge", {
      instance_id: c1,
      card: "4111 1111",
      amount: 25.0,
    }),
    "charge Ok carries the declared Bool",
  );
  checkOk(
    await call(client, "NotifyUser", { instance_id: c1, ok: true }),
    "notify_user Ok",
  );
  p = await call(client, "GetPartial", { instance_id: c1 });
  check(p.state === "FULFILLED", "c1 fulfilled");
  console.log("[bridge_client_node] state FULFILLED");

  s1.close();
  check(
    (await waitGone(client, c1, 5000)) >= 0,
    "closing c1's stream dropped its row",
  );

  // ---- the Err path and the automatic break ----

  const s2 = await openPaymentServiceSession(client, { expected_hash: pinned });
  const c2 = s2.signed.instance_id;
  check(s2.signed.currency === "USD", "c2 signs on the declared default");

  const out = await call(client, "Charge", {
    instance_id: c2,
    card: "4111 1111",
    amount: -2.0,
  });
  check(
    out.result === "err" && out.err === "41",
    "charge Err returns to the caller as a value, not an rpc error",
  );
  console.log(
    `[bridge_client_node] charge answered err=${out.err} on an OK rpc`,
  );

  p = await call(client, "GetPartial", { instance_id: c2 });
  check(p.state === "BROKEN", "the break line fired by itself");
  check(
    sliceEq(p.broken, ["Processing"]) &&
      sliceEq(p.pending, ["Validation", "notify_user"]),
    "c2 broken lists Processing",
  );
  check(
    (p.errors ?? []).length === 1 &&
      p.errors[0].pledge === "charge" &&
      p.errors[0].err === "41",
    "the automatic break kept the Err payload readable",
  );

  await expectCode(
    call(client, "ValidateCard", { instance_id: c2, card: "4111 1111" }),
    grpc.status.FAILED_PRECONDITION,
    "FAILED_PRECONDITION",
    "fulfillment after automatic break",
  );

  await call(client, "Break", { instance_id: c2 });
  await expectCode(
    call(client, "ValidateCard", { instance_id: c2, card: "4111 1111" }),
    grpc.status.FAILED_PRECONDITION,
    "FAILED_PRECONDITION",
    "fulfillment after an explicit break",
  );
  p = await call(client, "GetPartial", { instance_id: c2 });
  check(
    p.state === "BROKEN",
    "an explicitly broken instance still reads broken on its stream",
  );
  check(
    (p.errors ?? []).length === 0,
    "the explicit break zeroed the Err payloads, as it does in process",
  );

  s2.close();
  check(
    (await waitGone(client, c2, 5000)) >= 0,
    "closing a broken instance's stream dropped its row",
  );
  console.log("[bridge_client_node] the broken row left with its stream");

  // ---- an id nobody was issued, and a lying hash ----

  await expectCode(
    call(client, "GetPartial", { instance_id: "999999" }),
    grpc.status.NOT_FOUND,
    "NOT_FOUND",
    "unknown instance id",
  );

  await expectCode(
    openPaymentServiceSession(client, {
      expected_hash: (PaymentServiceShapeHash + 1n).toString(),
    }),
    grpc.status.ABORTED,
    "ABORTED",
    "a sign under a wrong shape hash",
  );

  // ---- a client that dies ----
  // A child process opens a session and is killed without a Break, without
  // a close, without saying anything. The dead stream is a fact and the
  // server has it in milliseconds.

  const self = fileURLToPath(import.meta.url);
  const child = spawn(process.execPath, [self, "--hold-session", "--addr", addr], {
    stdio: ["ignore", "pipe", "inherit"],
  });
  const doomed = await new Promise((resolve) => {
    child.stdout.once("data", (line) => resolve(String(line).trim()));
  });
  check(/^\d+$/.test(doomed), `the child opened a session, said ${doomed}`);

  child.kill("SIGKILL");
  const t0 = Date.now();
  const settled = await waitGone(client, doomed, 5000);
  check(settled >= 0, "a dead client's instance was collected");
  if (settled >= 0) {
    const elapsed = Date.now() - t0;
    console.log(
      `[bridge_client_node] client died, instance ${doomed} broken and dropped in ${elapsed}ms`,
    );
    check(
      elapsed < 1000,
      `the death was noticed in well under the old ${legacyTtlMs}ms timer, took ${elapsed}ms`,
    );
  }

  // ---- a client that is merely quiet ----

  const idleMs = legacyTtlMs * 3;
  const s3 = await openPaymentServiceSession(client, {
    currency: "EUR",
    expected_hash: pinned,
  });
  const c3 = s3.signed.instance_id;
  checkOk(
    await call(client, "ValidateCard", { instance_id: c3, card: "4111 1111" }),
    "the quiet instance's first pledge",
  );
  console.log(`[bridge_client_node] session ${c3} going quiet for ${idleMs}ms`);
  await sleep(idleMs);

  p = await call(client, "GetPartial", { instance_id: c3 });
  check(
    p.state === "SIGNED" && (p.fulfilled ?? []).length === 0,
    "the quiet instance kept its state",
  );
  checkOk(
    await call(client, "ValidateAmount", { instance_id: c3, amount: 25.0 }),
    "the quiet instance resumed its walk",
  );
  p = await call(client, "GetPartial", { instance_id: c3 });
  check(
    p.state === "PARTIAL",
    "Validation lands after the silence, on latches set before it",
  );
  console.log(
    `[bridge_client_node] session ${c3} resumed after ${idleMs}ms idle, state PARTIAL`,
  );

  s3.close();
  check(
    (await waitGone(client, c3, 5000)) >= 0,
    "the quiet instance left when its stream did",
  );
}

// The doomed child: opens a session, says which instance it got, and holds
// the process until something kills it.
async function holdSession(client) {
  const s = await openPaymentServiceSession(client, { currency: "EUR" });
  console.log(s.signed.instance_id);
  await new Promise(() => {});
}

async function main() {
  const client = makeClient();
  if (process.argv.includes("--hold-session")) {
    await holdSession(client);
    return;
  }
  await new Promise((resolve, reject) => {
    client.waitForReady(Date.now() + 10000, (err) =>
      err ? reject(err) : resolve(),
    );
  });
  await walk(client);
  if (failures) {
    process.exit(1);
  }
  console.log("[bridge_client_node] ok");
  process.exit(0);
}

main().catch((err) => {
  console.error(`[bridge_client_node] FAIL: ${err}`);
  process.exit(1);
});
