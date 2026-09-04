"""Entry point. Runs the HTTP app and the UDP listener in one process.

M0: serve /healthz and nothing else.
M4: bring up the UDP listener alongside.
"""

import os


def main() -> None:
    raise NotImplementedError(
        "M0: build the FastAPI app with /healthz, read config from env, "
        "and run it under uvicorn. See PLAN.md section 10."
    )


if __name__ == "__main__":
    main()
