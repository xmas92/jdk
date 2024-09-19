/*
 * Copyright (c) 2020, 2023, Oracle and/or its affiliates. All rights reserved.
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

package gc.z;

/*
 * @test TestExplicitHugePagesRetry
 * @requires vm.gc.ZGenerational
 * @requires os.family == "linux"
 * @summary Test ZGC with Explicit HugePages and
 * @library /test/lib
 * @run driver gc.z.TestExplicitHugePagesRetry
 */

import jdk.test.lib.Asserts;
import jdk.test.lib.os.linux.HugePageConfiguration;
import jdk.test.lib.process.ProcessTools;
import jdk.test.lib.process.OutputAnalyzer;
import jtreg.SkippedException;

import java.io.File;
import java.time.Instant;
import java.time.Duration;
import java.lang.Process;

public class TestExplicitHugePagesRetry {

    public static class WaitOnFile {
        public static void main(String[] args) throws Exception {
            if (args.length < 2) {
                throw new RuntimeException("No File Paths Specified");
            }
            String f1Path = args[0];
            File f1 = new File(f1Path);
            String f2Path = args[1];
            File f2 = new File(f2Path);
            if (!f1.exists() || !f2.exists()) {
                throw new RuntimeException("File does not exist");
            }
            f1.delete();
            waitUntilFileDeleted(f2, Duration.ofSeconds(10));
        }
    }

    static boolean waitUntilFileDeleted(File file, Duration timeout) throws Exception {
        Duration sleepTime = Duration.ofMillis(100);
        if (sleepTime.compareTo(timeout) < 0) {
            sleepTime = timeout;
        }
        Instant endTime = Instant.now().plus(timeout);
        while (Instant.now().isBefore(endTime)) {
            if (!file.exists()) {
                return true;
            }
            Thread.sleep(sleepTime);
        }
        return !file.exists();
    }

    static boolean waitUntilFileDeleted(File file) throws Exception {
        return waitUntilFileDeleted(file, Duration.ofSeconds(1));
    }

    static File createLockFile() throws Exception {
        File file = File.createTempFile(TestExplicitHugePagesRetry.class.getName(), ".lock");
        file.deleteOnExit();
        return file;
    }

    static final int ITERATIONS = 4;

    public static void main(String[] args) throws Exception {
        HugePageConfiguration config = HugePageConfiguration.readFromOS();
        if (!config.supportsExplicitHugePages()) {
            throw new SkippedException("Persistent Huge Pages are not configured and/or supported.");
        }
        long persistentHugePageSize = config.getExplicitDefaultHugePageSize();
        HugePageConfiguration.ExplicitHugePageStats stats = HugePageConfiguration.readExplicitHugePageStatsFromOS();

        long jvmHeapSize = persistentHugePageSize * (stats.free - stats.rsvd) / 2;

        if (jvmHeapSize < 0) {
            throw new RuntimeException("Invalid persistentHugePageSize {" +
                                        persistentHugePageSize +
                                        "} or ExplicitHugePageStats " +
                                        stats);
        }

        if (jvmHeapSize < 64 * 1024 * 1024) {
            throw new SkippedException("Not enough available persistent huge pages");
        }
        File f1 = createLockFile();
        File f2 = createLockFile();
        ProcessBuilder pb = ProcessTools.createLimitedTestJavaProcessBuilder(
                    "-XX:+UseZGC",
                    "-Xlog:gc+init=debug",
                    "-XX:+UseLargePages",
                    "-XX:+AlwaysPreTouch",
                    "-Xms" + jvmHeapSize,
                    "-Xmx" + jvmHeapSize,
                    WaitOnFile.class.getName(),
                    f1.getCanonicalPath(),
                    f2.getCanonicalPath()
            );
        Process p = pb.start();
        waitUntilFileDeleted(f1);

        boolean sawRetry = false;
        boolean sawSuccess = false;

        for (int i = 0; i < ITERATIONS; i++) {
            System.out.println(stats);
            System.out.println("Iteration: " + i);
            OutputAnalyzer output = ProcessTools.executeLimitedTestJava(
                    "-XX:+UseZGC",
                    "-Xlog:gc+init=debug",
                    "-XX:+UseLargePages",
                    "-XX:+AlwaysPreTouch",
                    "-XX:+UnlockDiagnosticVMOptions",
                    "-XX:+UseNewCode2",
                    "-Xms" + jvmHeapSize,
                    "-Xmx" + jvmHeapSize,
                    "-version"
                );
            System.out.println(HugePageConfiguration.readExplicitHugePageStatsFromOS());
            System.out.println();
            System.out.println(output.getOutput());
            if (output.getExitValue() == 0) {
                sawSuccess = true;
                break;
            } else {
                output.shouldMatch("Failed to commit memory \\([^\\(]+\\), retrying");
                sawRetry = true;
            }
            if (i == ITERATIONS / 2) {
                f2.delete();
            }
        }
        p.waitFor();
        Asserts.assertTrue(sawRetry, "Should have seen retry");
        if (!sawSuccess) {
            throw new SkippedException("Should have succeeded, but other concurrent processes may have used the resource");
        }
        System.out.println("waiter exitvalue: " + p.exitValue());
        System.out.println("success: " + sawSuccess);
        System.out.println("  retry: " + sawRetry);
    }
}
