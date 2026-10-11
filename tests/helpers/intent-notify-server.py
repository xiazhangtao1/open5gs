#!/usr/bin/env python3
"""Isolated h2c prior-knowledge fixture; pip install h2 before running.

Records one JSON line per request. Set --status or --delay to test failures.
This is a test fixture, not the compute-server implementation.
"""
import argparse
import asyncio
import json
import time
from h2.config import H2Configuration
from h2.connection import H2Connection
from h2.events import DataReceived, RequestReceived, StreamEnded
from h2.exceptions import StreamClosedError, ProtocolError


async def serve(reader, writer, args):
    conn = H2Connection(config=H2Configuration(client_side=False, header_encoding="utf-8"))
    conn.initiate_connection()
    writer.write(conn.data_to_send())
    streams = {}
    pending = set()

    async def respond(stream_id):
        await asyncio.sleep(args.delay)
        try:
            conn.send_headers(stream_id, [(":status", str(args.status))], end_stream=True)
            writer.write(conn.data_to_send())
            await writer.drain()
        except (ConnectionError, StreamClosedError, ProtocolError):
            pass

    try:
        while chunk := await reader.read(65536):
            for event in conn.receive_data(chunk):
                if isinstance(event, RequestReceived):
                    streams[event.stream_id] = {"headers": dict(event.headers), "data": bytearray()}
                elif isinstance(event, DataReceived):
                    streams[event.stream_id]["data"].extend(event.data)
                    conn.acknowledge_received_data(event.flow_controlled_length, event.stream_id)
                elif isinstance(event, StreamEnded):
                    request = streams.pop(event.stream_id)
                    record = {"receivedAt": int(time.time() * 1000),
                              "headers": request["headers"],
                              "body": json.loads(request["data"])}
                    with open(args.output, "a", encoding="utf-8") as output:
                        output.write(json.dumps(record) + "\n")
                    task = asyncio.create_task(respond(event.stream_id))
                    pending.add(task)
                    task.add_done_callback(pending.discard)
            writer.write(conn.data_to_send())
            await writer.drain()
    except (ConnectionError, asyncio.IncompleteReadError):
        pass
    finally:
        for task in pending:
            task.cancel()
        await asyncio.gather(*pending, return_exceptions=True)
        writer.close()
        try:
            await writer.wait_closed()
        except ConnectionError:
            pass


async def main(args):
    server = await asyncio.start_server(lambda r, w: serve(r, w, args), args.host, args.port)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--output", required=True)
    parser.add_argument("--status", type=int, default=204)
    parser.add_argument("--delay", type=float, default=0)
    asyncio.run(main(parser.parse_args()))
