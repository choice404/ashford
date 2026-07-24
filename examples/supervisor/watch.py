"""Reads the supervisor observer without acquiring a contract handle."""

import argparse
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, required=True)
    modes = parser.add_subparsers(dest="mode")
    modes.add_parser("list")
    get = modes.add_parser("get")
    get.add_argument("name")
    args = parser.parse_args(argv)
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    if args.mode is None:
        args.mode = "list"
    return args


def observer_modules():
    """Uses the generated host API, never an emitted contract surface."""
    try:
        import grpc
    except ImportError as error:
        raise RuntimeError("grpcio is required") from error
    sys.path.insert(0, str(ROOT / "target" / "grpc-gen"))
    try:
        import observer_pb2 as pb
        import observer_pb2_grpc as pb_grpc
    except ImportError as error:
        raise RuntimeError(
            "generated observer stubs are required in target/grpc-gen"
        ) from error
    return grpc, pb, pb_grpc


def row_line(row):
    return (f"{row.name} {row.state} pid {row.pid} run {row.run} "
            f"crashes {row.crashes}")


def main(argv=None):
    args = parse_args(argv)
    try:
        grpc, pb, pb_grpc = observer_modules()
    except RuntimeError as error:
        print(f"[watch] error {error}", file=sys.stderr, flush=True)
        return 1

    with grpc.insecure_channel(f"127.0.0.1:{args.port}") as channel:
        observer = pb_grpc.SupervisorObserverStub(channel)
        try:
            if args.mode == "list":
                reply = observer.ListServices(pb.ListRequest())
                for row in reply.services:
                    print(row_line(row), flush=True)
                return 0

            reply = observer.GetService(pb.GetRequest(name=args.name))
            print(row_line(reply.row), flush=True)
            print(f"fulfilled: {','.join(reply.fulfilled)}".rstrip(), flush=True)
            print(f"pending: {','.join(reply.pending)}".rstrip(), flush=True)
            print(f"broken: {','.join(reply.broken)}".rstrip(), flush=True)
            return 0
        except grpc.RpcError as error:
            print(error.details(), file=sys.stderr, flush=True)
            if error.code() == grpc.StatusCode.NOT_FOUND:
                return 3
            return 1


if __name__ == "__main__":
    raise SystemExit(main())
