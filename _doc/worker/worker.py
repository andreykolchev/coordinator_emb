import argparse
import json
import random
import time

import websocket


ESP32_WS_URL = "ws://192.168.4.1:81/"

WORKER_ID = "pi-zero-1"

def measure_temperature(inputs):
    # Simulated sensor measurement.
    return {
        "temperature_c": round(
            random.uniform(20.0, 35.0), 1
        )
    }


def measure_humidity(inputs):
    # Simulated sensor measurement.
    return {
        "humidity_rh": round(
            random.uniform(35.0, 90.0), 1
        )
    }


def ac_on(inputs):
    # Simulated actuator.
    print("AC ON")
    return {"ac": "on"}


def ac_off(inputs):
    # Simulated actuator.
    print("AC OFF")
    return {"ac": "off"}


TASK_HANDLERS = {
    "measure_temperature": measure_temperature,
    "measure_humidity": measure_humidity,
    "ac_on": ac_on,
    "ac_off": ac_off,
}

# Command-line options (see parse_args), set in main().
args = None


def send_json(ws, message):
    ws.send(json.dumps(message))


def on_open(ws):
    print("Connected to ESP32 coordinator")

    send_json(ws, {
        "type": "register",
        "worker_id": args.id,
        "capabilities": args.capabilities,
    })

    print("Worker registration sent")


def on_message(ws, message):
    try:
        data = json.loads(message)
    except json.JSONDecodeError:
        print("Invalid JSON received:", message)
        return

    if data.get("type") == "cancel_task":
        # Handlers run synchronously, so a running task cannot be interrupted.
        print(f"Ignoring cancel_task {data.get('task_id', '')}")
        return

    if data.get("type") != "task":
        return

    task_id = data.get("task_id", "")
    task_name = data.get("name", "")
    inputs = data.get("inputs") or {}

    print(f"Received task {task_id}: {task_name} {inputs}")

    if args.no_reply:
        print("Not replying (--no-reply)")
        return

    if args.delay_ms:
        time.sleep(args.delay_ms / 1000)

    def reply(message):
        message.update({
            "type": "task_result",
            "task_id": task_id,
        })
        send_json(ws, message)

    handler = TASK_HANDLERS.get(task_name)

    if handler is None:
        reply({
            "status": "failed",
            "result": {"error": f"Unsupported task: {task_name}"},
        })
        return

    if random.random() < args.fail_rate:
        reply({
            "status": "failed",
            "result": {"error": "simulated failure"},
        })
        return

    try:
        started = time.monotonic()

        output = handler(inputs)

        elapsed = time.monotonic() - started

        reply({
            "status": "completed",
            "result": output,
            "execution_time_ms": round(elapsed * 1000),
        })

        print("Task completed:", output)

    except Exception as exc:
        reply({
            "status": "failed",
            "result": {"error": str(exc)},
        })


def on_error(ws, error):
    print("WebSocket error:", error)


def on_close(ws, status_code, message):
    print("Disconnected:", status_code, message)


def parse_args():
    parser = argparse.ArgumentParser(description="Galaxy worker")
    parser.add_argument("--url", default=ESP32_WS_URL)
    parser.add_argument("--id", default=WORKER_ID)
    parser.add_argument("--capabilities", default=",".join(TASK_HANDLERS),
                        type=lambda text: [c for c in text.split(",") if c],
                        help="comma-separated task names")
    parser.add_argument("--delay-ms", type=int, default=0,
                        help="wait this long before answering a task")
    parser.add_argument("--fail-rate", type=float, default=0.0,
                        help="probability (0..1) that a task fails")
    parser.add_argument("--no-reply", action="store_true",
                        help="accept tasks but never answer (to test timeouts)")
    return parser.parse_args()


def main():
    global args
    args = parse_args()

    while True:
        try:
            print("Connecting to", args.url)

            ws = websocket.WebSocketApp(
                args.url,
                on_open=on_open,
                on_message=on_message,
                on_error=on_error,
                on_close=on_close,
            )

            ws.run_forever(
                ping_interval=20,
                ping_timeout=10,
            )

        except Exception as exc:
            print("Connection failed:", exc)

        print("Retrying in 3 seconds...")
        time.sleep(3)


if __name__ == "__main__":
    main()
