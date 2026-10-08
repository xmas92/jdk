/*
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

/*
 * @test
 * @summary Test fast JNI primitive getters
 * @requires vm.gc.Z
 * @library /test/lib
 * @run main/othervm/native -Xint -XX:+UseZGC -XX:+UseFastJNIAccessors -XX:+IgnoreUnrecognizedVMOptions -XX:-VerifyJNIFields -XX:-ZProactive -Xms256m -Xmx256m TestJNIFastGetFieldAfterGC
 */

import java.lang.ref.Reference;
import java.time.Duration;
import java.time.Instant;

import jdk.test.lib.Utils;

// This test exercises the different interactions between the StackWatermark,
// concurrent stack scanning and the fast implementations of JNI Get<Primitive>Field.
// The test runs multiple rounds of 10 scenarios:
// 1-4.  Wait and read directly using a parameter, local, global, or weak global reference.
// 5-8.  Wait in a callback, then read directly using the same four reference types.
// 9-10. Wait and read in a callback using a global or weak global reference.
public class TestJNIFastGetFieldAfterGC {
    private static final long EXPECTED = 0x12345678abcdef01L;
    private static final int ROUNDS = 20;
    private static final int SCENARIO_COUNT = 10;
    private static final Duration WORKER_TIMEOUT = Duration.ofMillis(Utils.adjustTimeout(30_000));

    private static volatile Object garbage;
    private static Thread worker;

    static {
        System.loadLibrary("TestJNIFastGetFieldAfterGC");
    }

    private static class Item {
        long value = EXPECTED;
    }

    private static native long readInNative(Item item, int scenario);
    private static native long readFromCallback();
    private static native boolean ready();
    private static native void resume();

    private static long nestedCallback() {
        // Start processing processes multiple frames, use the nestedHelpers
        // to ensure that we have multiple Java frames between the native frames.
        return nestedHelper1();
    }

    private static long nestedHelper1() {
        return nestedHelper2();
    }

    private static long nestedHelper2() {
        return nestedHelper3();
    }

    private static long nestedHelper3() {
        return readFromCallback();
    }

    private static void startWorkerThread(long[] result, Item item, int scenario) {
        worker = new Thread(() -> result[0] = readInNative(item, scenario));
        worker.setDaemon(true);
        worker.start();
    }

    private static void waitUntilWorkerReady() throws InterruptedException {
        final var deadline = Instant.now().plus(WORKER_TIMEOUT);
        while (!ready() && deadline.isAfter(Instant.now())) {
            Thread.sleep(Duration.ofMillis(1));
        }

        if (!ready()) {
            throw new AssertionError("Native worker did not become ready");
        }
    }

    private static void waitUntilWorkerFinished() throws InterruptedException {
        worker.join(WORKER_TIMEOUT);

        if (worker.isAlive()) {
            throw new AssertionError("Native worker did not finish");
        }
    }

    private static void generateGarbage() {
        final var arrays = new byte[3000][];
        for (int i = 0; i < arrays.length; i++) {
            arrays[i] = new byte[16 * 1024];
        }
        garbage = arrays;
    }

    private static void collectGarbage() {
        garbage = null;
        System.gc();
    }

    public static void main(String[] args) throws Exception {
        for (int scenario = 1; scenario <= SCENARIO_COUNT; scenario++) {
            for (int round = 0; round < ROUNDS; round++) {
                generateGarbage();

                final var result = new long[1];
                final var item = new Item();

                startWorkerThread(result, item, scenario);

                waitUntilWorkerReady();

                // Collect garbage and resume worker
                try {
                    collectGarbage();
                } finally {
                    resume();
                }

                waitUntilWorkerFinished();

                // Ensure the item is not collected until worker is done
                Reference.reachabilityFence(item);

                // Check the result
                if (result[0] != EXPECTED) {
                    throw new AssertionError("Scenario " + scenario + ", round " + round
                            + ": expected 0x" + Long.toHexString(EXPECTED)
                            + ", got 0x" + Long.toHexString(result[0]));
                }
            }
        }
    }
}
