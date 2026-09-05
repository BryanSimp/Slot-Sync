"""Token-bucket rate limiting -- PLAN.md section 10, M5.

Two callers with quite different shapes:

- The UDP listener checks *before* verifying the HMAC, because verifying is the
  expensive part and an unauthenticated flood should not get to spend it. The
  budget has to be generous: a 2 MiB push is 2048 datagrams arriving as fast as
  the sender can emit them, and throttling a legitimate console is worse than
  the flood.
- The HTTP layer limits *failed* authentication only, so a shared token on a LAN
  cannot be brute-forced. Successful requests are never throttled.
"""

from __future__ import annotations

import time


class TokenBucket:
    """A bucket that refills at `rate` per second, capped at `capacity`."""

    __slots__ = ("capacity", "rate", "tokens", "updated")

    def __init__(self, capacity: float, rate: float, now: float | None = None) -> None:
        self.capacity = float(capacity)
        self.rate = float(rate)
        self.tokens = float(capacity)
        self.updated = time.monotonic() if now is None else now

    def take(self, cost: float = 1.0, now: float | None = None) -> bool:
        """Spend `cost` tokens. False if the bucket is dry."""
        now = time.monotonic() if now is None else now
        elapsed = max(0.0, now - self.updated)
        self.tokens = min(self.capacity, self.tokens + elapsed * self.rate)
        self.updated = now

        if self.tokens < cost:
            return False
        self.tokens -= cost
        return True

    def idle_for(self, now: float) -> float:
        return now - self.updated


class RateLimiter:
    """One token bucket per key, with the key set kept bounded.

    The bucket table is itself an attack surface -- a spoofed source address per
    packet would otherwise grow it without limit -- so it is swept of idle
    entries and hard-capped.
    """

    def __init__(
        self,
        capacity: float,
        rate: float,
        *,
        max_keys: int = 1024,
        idle_timeout: float = 300.0,
    ) -> None:
        self.capacity = capacity
        self.rate = rate
        self.max_keys = max_keys
        self.idle_timeout = idle_timeout
        self._buckets: dict[object, TokenBucket] = {}

    def allow(self, key: object, cost: float = 1.0, now: float | None = None) -> bool:
        now = time.monotonic() if now is None else now
        bucket = self._buckets.get(key)

        if bucket is None:
            if len(self._buckets) >= self.max_keys:
                self._sweep(now)
            if len(self._buckets) >= self.max_keys:
                # Still full of active keys. Refusing is the safe answer: this
                # is already far outside anything a household produces.
                return False
            bucket = self._buckets[key] = TokenBucket(self.capacity, self.rate, now)

        return bucket.take(cost, now)

    def reset(self, key: object) -> None:
        """Forget a key. Used when an attempt succeeds and should stop counting."""
        self._buckets.pop(key, None)

    def _sweep(self, now: float) -> int:
        stale = [
            key
            for key, bucket in self._buckets.items()
            if bucket.idle_for(now) > self.idle_timeout
        ]
        for key in stale:
            del self._buckets[key]
        return len(stale)

    def __len__(self) -> int:
        return len(self._buckets)
