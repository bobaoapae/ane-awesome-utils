// SPDX-License-Identifier: BSD-3-Clause
//
// Drop-in replacement for org.xbill.DNS.AsyncSemaphore (dnsjava 3.6.x).
//
// Why: dnsjava's upstream AsyncSemaphore.acquire(Duration) puts a lambda
// inside a synchronized(queue) { ... } block. After D8 desugaring, the
// resulting bytecode trips the Android 6 (API 23) ART verifier with
// "[0x34] monitor-exit on non-object (Conflict)" because the verifier
// can't track that `queue` remains a valid Object reference across the
// synthetic-lambda-class boundary inside the monitor region.
//
// Failure surfaces as a process-killing VerifyError thrown the moment any
// dnsjava class touches AsyncSemaphore transitively (DohResolver,
// SimpleResolver, etc.) — observed live on a Samsung Galaxy J2 (Android
// 6.0.1, ARM64). Effects:
//   - the AIR app dies with SIG 9 ~1.5s after launch
//   - no DNS resolution available at all (system or DoH)
//   - tests fail before TestAgent steps run
//
// This file is on the source path of the ANE build, so the compiled
// .class lands in the same package as upstream's. The Android Gradle
// Plugin's R8/D8 dex merger picks the project source over the JAR
// dependency for matching FQNs, so this version replaces dnsjava's at
// dex time. Rest of dnsjava (DohResolver, SimpleResolver, Resolver, ...)
// keeps using the upstream JAR — only AsyncSemaphore is shadowed.
//
// Behaviour: identical to upstream. The synchronized block is split so
// the lambda lives outside the monitor region — verifier-friendly on
// Android 6, semantically the same:
//   - upstream: monitor-enter, mutate queue, build lambda capturing
//               queue+f, register lambda, monitor-exit
//   - patched:  monitor-enter, mutate queue, monitor-exit; THEN build
//               lambda + register. The lambda's queue.remove() is
//               re-synchronized in its own block (cheap — no contention
//               since the lambda fires on timeout, not on hot path).
package org.xbill.DNS;

import java.time.Duration;
import java.util.ArrayDeque;
import java.util.Queue;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;

final class AsyncSemaphore {
    private final Queue<CompletableFuture<Permit>> queue = new ArrayDeque<>();
    private final Permit singletonPermit = new Permit();
    private volatile int permits;

    final class Permit {
        public void release() {
            synchronized (queue) {
                CompletableFuture<Permit> next = queue.poll();
                if (next == null) {
                    permits++;
                } else {
                    next.complete(this);
                }
            }
        }
    }

    AsyncSemaphore(int permits) {
        this.permits = permits;
    }

    CompletionStage<Permit> acquire(Duration timeout) {
        // Fast path: permit available, return immediately.
        synchronized (queue) {
            if (permits > 0) {
                permits--;
                return CompletableFuture.completedFuture(singletonPermit);
            }
        }

        // Slow path: build the future + register OUTSIDE the original
        // synchronized region so the captured lambda doesn't sit inside a
        // monitor region (which is what trips the Android 6 verifier).
        final TimeoutCompletableFuture<Permit> f = new TimeoutCompletableFuture<>();
        f.compatTimeout(timeout.toNanos(), TimeUnit.NANOSECONDS)
                .whenComplete((result, ex) -> {
                    synchronized (queue) {
                        queue.remove(f);
                    }
                });

        // Re-acquire the lock to add to the queue. There IS a race window
        // between the first synchronized block above and this one — a
        // concurrent release() could now find the queue empty and increment
        // permits, leaving us blocked forever. Re-check permits inside this
        // critical section and consume one if available.
        synchronized (queue) {
            if (permits > 0) {
                permits--;
                f.complete(singletonPermit);
            } else {
                queue.add(f);
            }
        }
        return f;
    }
}
