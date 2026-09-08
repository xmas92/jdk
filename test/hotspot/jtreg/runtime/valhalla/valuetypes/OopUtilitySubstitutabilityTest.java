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
 * @test id=interpreter
 * @summary Test OopUtility substitutability against interpreted Java ==
 * @library /test/lib /
 * @enablePreview
 * @modules java.base/jdk.internal.value
 *          java.base/jdk.internal.vm.annotation
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -Xbootclasspath/a:. -Xint -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *      runtime.valhalla.valuetypes.OopUtilitySubstitutabilityTest
 */

/*
 * @test id=compiled
 * @summary Test compiled Java == against OopUtility substitutability
 * @library /test/lib /
 * @enablePreview
 * @modules java.base/jdk.internal.value
 *          java.base/jdk.internal.vm.annotation
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI
 *      runtime.valhalla.valuetypes.OopUtilitySubstitutabilityTest
 */

package runtime.valhalla.valuetypes;

import java.lang.reflect.Field;
import java.util.Random;

import jdk.internal.value.ValueClass;
import jdk.internal.vm.annotation.LooselyConsistentValue;
import jdk.internal.vm.annotation.NullRestricted;
import jdk.test.lib.Utils;
import jdk.test.whitebox.WhiteBox;

public class OopUtilitySubstitutabilityTest {
    private static final WhiteBox WB = WhiteBox.getWhiteBox();

    static value class Empty { }

    static value class OtherEmpty { }

    static value class Primitives {
        byte b;
        short s;
        char c;
        int i;
        long l;
        float f;
        double d;
        boolean z;

        Primitives(byte b, short s, char c, int i, long l, float f, double d, boolean z) {
            this.b = b;
            this.s = s;
            this.c = c;
            this.i = i;
            this.l = l;
            this.f = f;
            this.d = d;
            this.z = z;
        }
    }

    static value class Leaf {
        int x;

        Leaf(int x) {
            this.x = x;
        }
    }

    static value class Nested {
        @NullRestricted
        Leaf flat;
        Object reference;

        Nested(Leaf flat, Object reference) {
            this.flat = flat;
            this.reference = reference;
        }
    }

    @LooselyConsistentValue
    static value class NullableLeaf {
        int value;

        NullableLeaf(int value) {
            this.value = value;
        }
    }

    static value class NullableNested {
        NullableLeaf leaf;

        NullableNested(NullableLeaf leaf) {
            this.leaf = leaf;
        }
    }

    static abstract value class Base {
        int inherited;

        Base(int inherited) {
            this.inherited = inherited;
        }
    }

    static value class Derived extends Base {
        int own;

        Derived(int inherited, int own) {
            this.own = own;
            super(inherited);
        }
    }

    static value class WithStatic {
        static int ignored;
        int value;

        WithStatic(int value) {
            this.value = value;
        }
    }

    static value class RandomPayload {
        byte b;
        short s;
        char c;
        int i;
        long l;
        float f;
        double d;
        boolean z;
        @NullRestricted
        Leaf flat;
        NullableLeaf nullable;
        Object reference;

        RandomPayload(byte b, short s, char c, int i, long l, float f, double d,
                      boolean z, Leaf flat, NullableLeaf nullable, Object reference) {
            this.b = b;
            this.s = s;
            this.c = c;
            this.i = i;
            this.l = l;
            this.f = f;
            this.d = d;
            this.z = z;
            this.flat = flat;
            this.nullable = nullable;
            this.reference = reference;
        }
    }

    private static final Object[] RANDOM_REFERENCES = {
        null, new Object(), new Object(), new Leaf(1), new Leaf(1), new Leaf(2),
        new int[] { 1 }, new int[] { 1 }
    };

    public static void main(String[] args) throws ReflectiveOperationException {
        testOperands();
        testPrimitiveFields();
        testNestedFields();
        testNullableField();
        testInheritedAndStaticFields();
        testRandomPayloads();
        if (isEnabled("UseFieldFlattening") && isEnabled("UseNullFreeAtomicValueFlattening")) {
            testNullRestrictedFlatField();
        }
        if (isEnabled("UseFieldFlattening") && isEnabled("UseNullableAtomicValueFlattening")) {
            testNullableFlatField();
        }
        if (isEnabled("UseArrayFlattening") && isEnabled("UseNullFreeNonAtomicValueFlattening")) {
            testNullRestrictedFlatArray();
        }
        if (isEnabled("UseArrayFlattening") && isEnabled("UseNullableAtomicValueFlattening")) {
            testNullableFlatArray();
        }
    }

    private static boolean isEnabled(String name) {
        return Boolean.TRUE.equals(WB.getBooleanVMFlag(name));
    }

    private static void testOperands() {
        Object identity = new Object();
        Empty empty = new Empty();

        verifyJavaEquals(null, null);
        verifyJavaEquals(identity, identity);
        verifyJavaEquals(empty, empty);
        verifyJavaEquals(new Empty(), new Empty());

        verifyJavaEquals(null, identity);
        verifyJavaEquals(identity, null);
        verifyJavaEquals(new Object(), new Object());
        verifyJavaEquals(new Empty(), new OtherEmpty());
    }

    private static void testPrimitiveFields() {
        Primitives p = primitives(0.0f, 0.0d);
        verifyJavaEquals(p, primitives(0.0f, 0.0d));

        verifyJavaEquals(p, new Primitives((byte) 2, (short) 3, 'x', 4, 5L, 0.0f, 0.0d, true));
        verifyJavaEquals(p, new Primitives((byte) 1, (short) 4, 'x', 4, 5L, 0.0f, 0.0d, true));
        verifyJavaEquals(p, new Primitives((byte) 1, (short) 3, 'y', 4, 5L, 0.0f, 0.0d, true));
        verifyJavaEquals(p, new Primitives((byte) 1, (short) 3, 'x', 6, 5L, 0.0f, 0.0d, true));
        verifyJavaEquals(p, new Primitives((byte) 1, (short) 3, 'x', 4, 6L, 0.0f, 0.0d, true));
        verifyJavaEquals(p, new Primitives((byte) 1, (short) 3, 'x', 4, 5L, 1.0f, 0.0d, true));
        verifyJavaEquals(p, new Primitives((byte) 1, (short) 3, 'x', 4, 5L, 0.0f, 1.0d, true));
        verifyJavaEquals(p, new Primitives((byte) 1, (short) 3, 'x', 4, 5L, 0.0f, 0.0d, false));

        verifyJavaEquals(primitives(0.0f, 0.0d), primitives(-0.0f, 0.0d));
        verifyJavaEquals(primitives(0.0f, 0.0d), primitives(0.0f, -0.0d));

        float floatNaN = Float.intBitsToFloat(0x7fc00001);
        double doubleNaN = Double.longBitsToDouble(0x7ff8000000000001L);
        verifyJavaEquals(primitives(floatNaN, doubleNaN), primitives(floatNaN, doubleNaN));
        verifyJavaEquals(primitives(floatNaN, doubleNaN),
                         primitives(Float.intBitsToFloat(0x7fc00002), doubleNaN));
        verifyJavaEquals(primitives(floatNaN, doubleNaN),
                         primitives(floatNaN, Double.longBitsToDouble(0x7ff8000000000002L)));
    }

    private static Primitives primitives(float f, double d) {
        return new Primitives((byte) 1, (short) 3, 'x', 4, 5L, f, d, true);
    }

    private static void testNestedFields() {
        Object identity = new Object();
        verifyJavaEquals(new Nested(new Leaf(42), identity), new Nested(new Leaf(42), identity));
        verifyJavaEquals(new Nested(new Leaf(42), identity), new Nested(new Leaf(43), identity));
        verifyJavaEquals(new Nested(new Leaf(42), new Object()), new Nested(new Leaf(42), new Object()));

        // A value object stored in an Object field uses recursive substitutability.
        verifyJavaEquals(new Nested(new Leaf(42), new Leaf(7)),
                         new Nested(new Leaf(42), new Leaf(7)));
        verifyJavaEquals(new Nested(new Leaf(42), new Leaf(7)),
                         new Nested(new Leaf(42), new Leaf(8)));

        int[] array = { 1 };
        verifyJavaEquals(new Nested(new Leaf(42), array), new Nested(new Leaf(42), array));
        verifyJavaEquals(new Nested(new Leaf(42), new int[] { 1 }),
                         new Nested(new Leaf(42), new int[] { 1 }));
    }

    private static void testNullableField() {
        verifyJavaEquals(new NullableNested(null), new NullableNested(null));
        verifyJavaEquals(new NullableNested(null), new NullableNested(new NullableLeaf(0)));
        verifyJavaEquals(new NullableNested(new NullableLeaf(11)),
                         new NullableNested(new NullableLeaf(11)));
        verifyJavaEquals(new NullableNested(new NullableLeaf(11)),
                         new NullableNested(new NullableLeaf(12)));
    }

    private static void testInheritedAndStaticFields() {
        verifyJavaEquals(new Derived(1, 2), new Derived(1, 2));
        verifyJavaEquals(new Derived(1, 2), new Derived(3, 2));
        verifyJavaEquals(new Derived(1, 2), new Derived(1, 3));

        WithStatic first = new WithStatic(9);
        WithStatic.ignored = 1;
        WithStatic second = new WithStatic(9);
        WithStatic.ignored = 2;
        verifyJavaEquals(first, second);
    }

    private static void testRandomPayloads() {
        Random random = Utils.getRandomInstance();
        for (int i = 0; i < 10_000; i++) {
            RandomPayload a = randomPayload(random);
            RandomPayload b = random.nextBoolean() ? copyOf(a) : randomPayload(random);
            verifyJavaEquals(a, b);
        }
    }

    private static RandomPayload randomPayload(Random random) {
        NullableLeaf nullable = random.nextBoolean() ? null : new NullableLeaf(random.nextInt());
        Object reference = RANDOM_REFERENCES[random.nextInt(RANDOM_REFERENCES.length)];
        return new RandomPayload((byte) random.nextInt(), (short) random.nextInt(),
                                 (char) random.nextInt(), random.nextInt(), random.nextLong(),
                                 Float.intBitsToFloat(random.nextInt()),
                                 Double.longBitsToDouble(random.nextLong()), random.nextBoolean(),
                                 new Leaf(random.nextInt()), nullable, reference);
    }

    private static RandomPayload copyOf(RandomPayload value) {
        return new RandomPayload(value.b, value.s, value.c, value.i, value.l, value.f, value.d,
                                 value.z, value.flat, value.nullable, value.reference);
    }

    private static void testNullRestrictedFlatField() throws ReflectiveOperationException {
        Field flat = Nested.class.getDeclaredField("flat");
        Object identity = new Object();
        Nested a = new Nested(new Leaf(1), identity);
        Nested b = new Nested(new Leaf(1), identity);
        Nested c = new Nested(new Leaf(2), identity);
        verifyFlatFields(a, b, flat, a.flat, b.flat);
        verifyFlatFields(a, c, flat, a.flat, c.flat);
    }

    private static void testNullableFlatField() throws ReflectiveOperationException {
        Field nullable = NullableNested.class.getDeclaredField("leaf");
        NullableNested nullA = new NullableNested(null);
        NullableNested nullB = new NullableNested(null);
        NullableNested nonNullA = new NullableNested(new NullableLeaf(11));
        NullableNested nonNullB = new NullableNested(new NullableLeaf(11));
        verifyFlatFields(nullA, nullB, nullable, nullA.leaf, nullB.leaf);
        verifyFlatFields(nullA, nonNullA, nullable, nullA.leaf, nonNullA.leaf);
        verifyFlatFields(nonNullA, nonNullB, nullable, nonNullA.leaf, nonNullB.leaf);
    }

    private static void testNullRestrictedFlatArray() {
        Leaf[] arrayA = (Leaf[]) ValueClass.newNullRestrictedNonAtomicArray(Leaf.class, 3, new Leaf(0));
        Leaf[] arrayB = (Leaf[]) ValueClass.newNullRestrictedNonAtomicArray(Leaf.class, 3, new Leaf(0));
        arrayA[0] = new Leaf(1);
        arrayA[1] = new Leaf(2);
        arrayB[0] = new Leaf(1);
        arrayB[1] = new Leaf(3);
        verifyFlatArrayElements(arrayA, 0, arrayB, 0, arrayA[0], arrayB[0]);
        verifyFlatArrayElements(arrayA, 1, arrayB, 1, arrayA[1], arrayB[1]);
    }

    private static void testNullableFlatArray() {
        NullableLeaf[] nullableA = (NullableLeaf[]) ValueClass.newNullableAtomicArray(NullableLeaf.class, 3);
        NullableLeaf[] nullableB = (NullableLeaf[]) ValueClass.newNullableAtomicArray(NullableLeaf.class, 3);
        nullableA[1] = new NullableLeaf(7);
        nullableB[1] = new NullableLeaf(7);
        verifyFlatArrayElements(nullableA, 0, nullableB, 0, nullableA[0], nullableB[0]);
        verifyFlatArrayElements(nullableA, 0, nullableB, 1, nullableA[0], nullableB[1]);
        verifyFlatArrayElements(nullableA, 1, nullableB, 1, nullableA[1], nullableB[1]);
    }

    private static void verifyJavaEquals(Object a, Object b) {
        boolean expected = a == b;
        boolean actual = WB.javaEqualsOperator(a, b);
        boolean reverseExpected = b == a;
        boolean reverseActual = WB.javaEqualsOperator(b, a);
        if (actual != expected || reverseActual != reverseExpected) {
            throw new AssertionError("javaEqualsOperator mismatch: " + a + " and " + b
                                     + ", expected " + expected + ", got " + actual
                                     + "; reverse expected " + reverseExpected + ", got " + reverseActual);
        }
    }

    private static void verifyFlatFields(Object a, Object b, Field field,
                                         Object fieldA, Object fieldB) {
        boolean expected = fieldA == fieldB;
        boolean actual = WB.areFlatFieldsSubstitutable(a, b, field);
        if (actual != expected) {
            throw new AssertionError("Flat field mismatch for " + field + ": expected "
                                     + expected + ", got " + actual);
        }
    }

    private static void verifyFlatArrayElements(Object a, int aIndex, Object b, int bIndex,
                                                Object elementA, Object elementB) {
        boolean expected = elementA == elementB;
        boolean actual = WB.areFlatArrayElementsSubstitutable(a, aIndex, b, bIndex);
        if (actual != expected) {
            throw new AssertionError("Flat array element mismatch: expected " + expected
                                     + ", got " + actual);
        }
    }
}
