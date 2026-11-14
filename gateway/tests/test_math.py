from fractions import Fraction

import pytest

from robot_gateway.tutor.mathcheck import correct_claims, extract_number, find_arithmetic


@pytest.mark.parametrize("text,value", [
    ("what is 7 + 5", 12), ("seven plus five", 12), ("what's twelve times twelve", 144),
    ("100 divided by 4", 25), ("10 - 3", 7), ("2 + 3 * 4", 14), ("(2 + 3) * 4", 20),
    ("كم سبعة زائد خمسة", 12), ("٣ + ٤", 7), ("كم 6 ضرب 7", 42), ("twenty-one minus one", 20),
    ("1/2 + 1/4", Fraction(3, 4)),
])
def test_find_arithmetic(text, value):
    ev = find_arithmetic(text)
    assert ev is not None and ev.value == value


@pytest.mark.parametrize("text", ["I am 6 years old", "hello", "my cat has 4 legs", "5 / 0"])
def test_not_arithmetic_or_invalid(text):
    assert find_arithmetic(text) is None


def test_extract_number():
    assert extract_number("it's twelve!") == 12
    assert extract_number("اثنا عشر") == 12
    assert extract_number("١٥") == 15
    assert extract_number("no idea") is None


def test_correct_claims_fixes_wrong_results_only():
    fixed, n = correct_claims("Yes! 5 + 5 = 11, and 2 x 3 = 6.")
    assert n == 1 and "5 + 5 = 10" in fixed and "2 x 3 = 6" in fixed
