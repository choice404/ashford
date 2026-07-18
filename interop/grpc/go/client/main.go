// bridge_client_go: the demo_payment.py walk, driven from Go against the
// bridge server, built from nothing but ashc's emitted artifacts. The
// paymentpb package this imports is protoc's output over the emitted
// payment.proto plus the emitted session wrapper, so every symbol the walk
// touches, the typed pledge calls, the signature, the pinned shape hash,
// and the session handle, came out of the compiler. No Ashford binding,
// no libashrt, no hand written stub.
//
// The walk asserts what bridge_client.py asserts, minus the Debug rpc,
// which the emitted surface does not carry because the instance table is
// server internal: where the Python client counts rows, this one reads the
// row's own fate through GetPartial, present until its stream ends and
// NOT_FOUND after. The sign calls pin the wrapper's shape hash, so a run
// that reaches the first pledge has already proven the emitted hash is the
// one the compiled module registered.
package main

import (
	"bufio"
	"context"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"

	"ashbridge/paymentpb"
)

var failures int

func check(cond bool, what string) {
	if !cond {
		fmt.Fprintf(os.Stderr, "[bridge_client_go] FAIL: %s\n", what)
		failures = 1
	}
}

// expectCode demands a gRPC error with a given code. A pledge's Err never
// lands here: only a fulfillment that did not run does.
func expectCode(err error, want codes.Code, what string) {
	if err == nil {
		check(false, fmt.Sprintf("%s did not fail", what))
		return
	}
	st, ok := status.FromError(err)
	if !ok {
		check(false, fmt.Sprintf("%s failed outside gRPC: %v", what, err))
		return
	}
	check(st.Code() == want,
		fmt.Sprintf("%s is %v, got %v", what, want, st.Code()))
}

func resultOk(r *paymentpb.BoolIntResult) (bool, bool) {
	if v, isOk := r.GetResult().(*paymentpb.BoolIntResult_Ok); isOk {
		return v.Ok, true
	}
	return false, false
}

func resultErr(r *paymentpb.BoolIntResult) (int64, bool) {
	if v, isErr := r.GetResult().(*paymentpb.BoolIntResult_Err); isErr {
		return v.Err, true
	}
	return 0, false
}

func checkOk(r *paymentpb.BoolIntResult, err error, what string) {
	if err != nil {
		check(false, fmt.Sprintf("%s: %v", what, err))
		return
	}
	v, isOk := resultOk(r)
	check(isOk && v, what)
}

func sliceEq(got []string, want ...string) bool {
	if len(got) != len(want) {
		return false
	}
	for i := range got {
		if got[i] != want[i] {
			return false
		}
	}
	return true
}

// waitGone polls an instance id until it answers NOT_FOUND, and answers how
// long that took. GetPartial reads without touching the instance, so the
// poll cannot itself change what is being measured.
func waitGone(ctx context.Context, c paymentpb.PaymentServiceClient, id uint64, timeout time.Duration) (time.Duration, bool) {
	t0 := time.Now()
	deadline := t0.Add(timeout)
	for time.Now().Before(deadline) {
		_, err := c.GetPartial(ctx, &paymentpb.PartialRequest{InstanceId: id})
		if err != nil {
			if st, ok := status.FromError(err); ok && st.Code() == codes.NotFound {
				return time.Since(t0), true
			}
			return 0, false
		}
		time.Sleep(5 * time.Millisecond)
	}
	return 0, false
}

func walk(ctx context.Context, c paymentpb.PaymentServiceClient, legacyTTL time.Duration) {
	// ---- the partial path, signed with a vow override and a pinned hash ----

	s1, err := paymentpb.OpenPaymentServiceSession(ctx, c, &paymentpb.SignRequest{
		Currency:     proto.String("EUR"),
		ExpectedHash: paymentpb.PaymentServiceShapeHash,
	})
	if err != nil {
		check(false, fmt.Sprintf("the pinned sign refused: %v", err))
		return
	}
	c1 := s1.Signed.GetInstanceId()
	check(c1 != 0, "the session stream issued an instance id")
	check(s1.Signed.GetCurrency() == "EUR", "vow override landed across the wire")
	check(s1.Signed.GetShapeHash() == paymentpb.PaymentServiceShapeHash,
		"the signature carries the hash the wrapper pinned")
	check(s1.Signed.GetSignedAt() > 0, "c1 carries a timestamp")
	fmt.Printf("[bridge_client_go] session %d open, currency %s, shape 0x%x\n",
		c1, s1.Signed.GetCurrency(), s1.Signed.GetShapeHash())

	p, err := c.GetPartial(ctx, &paymentpb.PartialRequest{InstanceId: c1})
	check(err == nil && sliceEq(p.GetPending(), "Validation", "Processing", "notify_user"),
		"c1 pending order is subs then loose pledges")

	r, err := c.ValidateCard(ctx, &paymentpb.ValidateCardRequest{InstanceId: c1, Card: "4111 1111"})
	checkOk(r, err, "validate_card Ok")
	p, err = c.GetPartial(ctx, &paymentpb.PartialRequest{InstanceId: c1})
	check(err == nil && p.GetState() == paymentpb.ContractState_SIGNED,
		"half a subcontract moves nothing")

	r, err = c.ValidateAmount(ctx, &paymentpb.ValidateAmountRequest{InstanceId: c1, Amount: 25.0})
	checkOk(r, err, "validate_amount Ok")
	p, err = c.GetPartial(ctx, &paymentpb.PartialRequest{InstanceId: c1})
	check(err == nil && p.GetState() == paymentpb.ContractState_PARTIAL,
		"Validation lands, c1 partial")
	check(err == nil && sliceEq(p.GetFulfilled(), "Validation") &&
		sliceEq(p.GetPending(), "Processing", "notify_user") &&
		len(p.GetBroken()) == 0 && len(p.GetErrors()) == 0,
		"partial names after Validation")
	fmt.Printf("[bridge_client_go] state PARTIAL, fulfilled %v, pending %v\n",
		p.GetFulfilled(), p.GetPending())

	// charge is the Python body behind the abstract pledge: the answer
	// crosses three boundaries now, the C ABI into Python, Python out over
	// gRPC, and the wire into Go.
	r, err = c.Charge(ctx, &paymentpb.ChargeRequest{InstanceId: c1, Card: "4111 1111", Amount: 25.0})
	checkOk(r, err, "charge Ok carries the declared Bool")
	r, err = c.NotifyUser(ctx, &paymentpb.NotifyUserRequest{InstanceId: c1, Ok: true})
	checkOk(r, err, "notify_user Ok")

	p, err = c.GetPartial(ctx, &paymentpb.PartialRequest{InstanceId: c1})
	check(err == nil && p.GetState() == paymentpb.ContractState_FULFILLED, "c1 fulfilled")
	check(err == nil && len(p.GetFulfilled()) == 3 && len(p.GetPending()) == 0 &&
		len(p.GetErrors()) == 0, "every item fulfilled")
	fmt.Println("[bridge_client_go] state FULFILLED")

	// A fulfilled instance leaves with its stream the same as any other.
	s1.Close()
	_, gone := waitGone(ctx, c, c1, 5*time.Second)
	check(gone, "closing c1's stream dropped its row")

	// ---- the Err path and the automatic break ----
	// The contract's Err is a value: an OK rpc carrying err=41.

	s2, err := paymentpb.OpenPaymentServiceSession(ctx, c, &paymentpb.SignRequest{
		ExpectedHash: paymentpb.PaymentServiceShapeHash,
	})
	if err != nil {
		check(false, fmt.Sprintf("the default sign refused: %v", err))
		return
	}
	c2 := s2.Signed.GetInstanceId()
	check(s2.Signed.GetCurrency() == "USD", "c2 signs on the declared default")

	r, err = c.Charge(ctx, &paymentpb.ChargeRequest{InstanceId: c2, Card: "4111 1111", Amount: -2.0})
	if err != nil {
		check(false, fmt.Sprintf("charge Err crossed as a transport error: %v", err))
	} else {
		ev, isErr := resultErr(r)
		check(isErr && ev == 41,
			"charge Err returns to the caller as a value, not an rpc error")
		fmt.Printf("[bridge_client_go] charge answered err=%d on an OK rpc\n", ev)
	}

	p, err = c.GetPartial(ctx, &paymentpb.PartialRequest{InstanceId: c2})
	check(err == nil && p.GetState() == paymentpb.ContractState_BROKEN,
		"the break line fired by itself")
	check(err == nil && sliceEq(p.GetBroken(), "Processing") &&
		sliceEq(p.GetPending(), "Validation", "notify_user"),
		"c2 broken lists Processing")
	check(err == nil && len(p.GetErrors()) == 1 &&
		p.GetErrors()[0].GetPledge() == "charge" && p.GetErrors()[0].GetErr() == 41,
		"the automatic break kept the Err payload readable")

	// Fulfillment against a broken instance is ASH_ERR_STATE, the one
	// Ashford status this walk turns into a gRPC error.
	_, err = c.ValidateCard(ctx, &paymentpb.ValidateCardRequest{InstanceId: c2, Card: "4111 1111"})
	expectCode(err, codes.FailedPrecondition, "fulfillment after automatic break")

	// An explicit break, then a fulfillment, is the same state error; the
	// row outlives the break behind its stream, and what it reads matches
	// the in process host down to the zeroed Err payloads.
	_, err = c.Break(ctx, &paymentpb.BreakRequest{InstanceId: c2})
	check(err == nil, "the explicit break lands")
	_, err = c.ValidateCard(ctx, &paymentpb.ValidateCardRequest{InstanceId: c2, Card: "4111 1111"})
	expectCode(err, codes.FailedPrecondition, "fulfillment after an explicit break")

	p, err = c.GetPartial(ctx, &paymentpb.PartialRequest{InstanceId: c2})
	check(err == nil && p.GetState() == paymentpb.ContractState_BROKEN,
		"an explicitly broken instance still reads broken on its stream")
	check(err == nil && sliceEq(p.GetBroken(), "Processing") &&
		sliceEq(p.GetPending(), "Validation", "notify_user"),
		"the explicit break kept the partial names readable")
	check(err == nil && len(p.GetErrors()) == 0,
		"the explicit break zeroed the Err payloads, as it does in process")

	s2.Close()
	_, gone = waitGone(ctx, c, c2, 5*time.Second)
	check(gone, "closing a broken instance's stream dropped its row")
	fmt.Println("[bridge_client_go] the broken row left with its stream")

	// ---- an id nobody was issued ----

	_, err = c.GetPartial(ctx, &paymentpb.PartialRequest{InstanceId: 999999})
	expectCode(err, codes.NotFound, "unknown instance id")

	// ---- a lying hash ----
	// The wrapper's pinned hash is what an honest consumer sends; a wrong
	// one is refused at sign, before an instance exists, and the refusal is
	// a transport error because no contract ever ran. Shape skew is
	// ASH_ERR_VERSION in the runtime and the bridge maps it to ABORTED.

	_, err = paymentpb.OpenPaymentServiceSession(ctx, c, &paymentpb.SignRequest{
		ExpectedHash: paymentpb.PaymentServiceShapeHash + 1,
	})
	expectCode(err, codes.Aborted, "a sign under a wrong shape hash")

	// ---- a client that dies ----
	// A child process opens a session and is killed without a Break, without
	// a close, without saying anything. The dead stream is a fact and the
	// server has it in milliseconds.

	self, err := os.Executable()
	if err != nil {
		check(false, fmt.Sprintf("no path to self: %v", err))
		return
	}
	child := exec.Command(self, "-hold-session", "-addr", flagAddr)
	childOut, err := child.StdoutPipe()
	if err != nil {
		check(false, fmt.Sprintf("no pipe to the child: %v", err))
		return
	}
	if err := child.Start(); err != nil {
		check(false, fmt.Sprintf("the child did not start: %v", err))
		return
	}
	var doomed uint64
	if _, err := fmt.Fscan(bufio.NewReader(childOut), &doomed); err != nil {
		check(false, fmt.Sprintf("the child said no id: %v", err))
	}
	check(doomed != 0, "the child opened a session")

	_ = child.Process.Kill()
	t0 := time.Now()
	_ = child.Wait()
	_, gone = waitGone(ctx, c, doomed, 5*time.Second)
	elapsed := time.Since(t0)
	check(gone, "a dead client's instance was collected")
	if gone {
		fmt.Printf("[bridge_client_go] client died, instance %d broken and dropped in %dms\n",
			doomed, elapsed.Milliseconds())
		check(elapsed < time.Second,
			fmt.Sprintf("the death was noticed in well under the old %v timer, took %v",
				legacyTTL, elapsed))
	}

	// ---- a client that is merely quiet ----
	// The contract waiting on something slow: it holds its stream, says
	// nothing for several times the old timer, and keeps its instance.

	idle := legacyTTL * 3
	s3, err := paymentpb.OpenPaymentServiceSession(ctx, c, &paymentpb.SignRequest{
		Currency:     proto.String("EUR"),
		ExpectedHash: paymentpb.PaymentServiceShapeHash,
	})
	if err != nil {
		check(false, fmt.Sprintf("the quiet sign refused: %v", err))
		return
	}
	c3 := s3.Signed.GetInstanceId()
	r, err = c.ValidateCard(ctx, &paymentpb.ValidateCardRequest{InstanceId: c3, Card: "4111 1111"})
	checkOk(r, err, "the quiet instance's first pledge")
	fmt.Printf("[bridge_client_go] session %d going quiet for %v\n", c3, idle)
	time.Sleep(idle)

	p, err = c.GetPartial(ctx, &paymentpb.PartialRequest{InstanceId: c3})
	check(err == nil && p.GetState() == paymentpb.ContractState_SIGNED &&
		len(p.GetFulfilled()) == 0, "the quiet instance kept its state")

	// And it still works: the latch validate_card set before the silence is
	// still set, and the walk resumes.
	r, err = c.ValidateAmount(ctx, &paymentpb.ValidateAmountRequest{InstanceId: c3, Amount: 25.0})
	checkOk(r, err, "the quiet instance resumed its walk")
	p, err = c.GetPartial(ctx, &paymentpb.PartialRequest{InstanceId: c3})
	check(err == nil && p.GetState() == paymentpb.ContractState_PARTIAL,
		"Validation lands after the silence, on latches set before it")
	fmt.Printf("[bridge_client_go] session %d resumed after %v idle, state PARTIAL\n", c3, idle)

	s3.Close()
	_, gone = waitGone(ctx, c, c3, 5*time.Second)
	check(gone, "the quiet instance left when its stream did")
}

// holdSession is the doomed child: it opens a session, says which instance
// it got, and holds the process until something kills it. It never breaks,
// never closes, and never gets a chance to.
func holdSession(ctx context.Context, c paymentpb.PaymentServiceClient) int {
	s, err := paymentpb.OpenPaymentServiceSession(ctx, c, &paymentpb.SignRequest{
		Currency: proto.String("EUR"),
	})
	if err != nil {
		fmt.Fprintf(os.Stderr, "[bridge_client_go] the held sign refused: %v\n", err)
		return 1
	}
	fmt.Println(s.Signed.GetInstanceId())
	select {}
}

var flagAddr string

func main() {
	hold := flag.Bool("hold-session", false, "internal: open a session and hold it until killed")
	legacy := flag.Duration("legacy-ttl", 2*time.Second, "the idle timer step 1's server ran, kept as the yardstick")
	flag.StringVar(&flagAddr, "addr", "127.0.0.1:50251", "the bridge server's address")
	flag.Parse()

	conn, err := grpc.NewClient(flagAddr,
		grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		fmt.Fprintf(os.Stderr, "[bridge_client_go] FAIL: no channel: %v\n", err)
		os.Exit(1)
	}
	defer conn.Close()
	c := paymentpb.NewPaymentServiceClient(conn)
	ctx := context.Background()

	if *hold {
		os.Exit(holdSession(ctx, c))
	}

	// The server may still be binding; the first read retries until it
	// answers or ten seconds pass.
	up := false
	for deadline := time.Now().Add(10 * time.Second); time.Now().Before(deadline); {
		_, err := c.GetPartial(ctx, &paymentpb.PartialRequest{InstanceId: 1})
		if st, ok := status.FromError(err); ok && st.Code() != codes.Unavailable {
			up = true
			break
		}
		time.Sleep(50 * time.Millisecond)
	}
	if !up {
		fmt.Fprintln(os.Stderr, "[bridge_client_go] FAIL: server never came up")
		os.Exit(1)
	}

	walk(ctx, c, *legacy)

	if failures != 0 {
		os.Exit(1)
	}
	fmt.Println("[bridge_client_go] ok")
}
