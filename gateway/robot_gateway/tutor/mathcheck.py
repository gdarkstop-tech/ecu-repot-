"""Deterministic arithmetic for children's questions and for checking model output.

Arithmetic answers are always COMPUTED here (exact rational arithmetic), never taken from a
language model. The parser understands digits, Arabic-Indic digits, and English/Arabic number
and operator words ("seven plus five", "سبعة زائد خمسة", "7 x 8", "٣ + ٤").
"""
from __future__ import annotations

import re
from dataclasses import dataclass
from fractions import Fraction

ARABIC_INDIC = str.maketrans("٠١٢٣٤٥٦٧٨٩۰۱۲۳۴۵۶۷۸۹", "01234567890123456789")

EN_UNITS = {
    "zero": 0, "one": 1, "two": 2, "three": 3, "four": 4, "five": 5, "six": 6, "seven": 7,
    "eight": 8, "nine": 9, "ten": 10, "eleven": 11, "twelve": 12, "thirteen": 13,
    "fourteen": 14, "fifteen": 15, "sixteen": 16, "seventeen": 17, "eighteen": 18, "nineteen": 19,
}
EN_TENS = {"twenty": 20, "thirty": 30, "forty": 40, "fifty": 50, "sixty": 60, "seventy": 70,
           "eighty": 80, "ninety": 90}

# Arabic number words (common spellings, with and without hamza/taa marbuta variants).
AR_NUMBERS = {
    "صفر": 0, "واحد": 1, "واحدة": 1, "اثنان": 2, "اثنين": 2, "إثنين": 2, "اثنتان": 2,
    "ثلاثة": 3, "ثلاث": 3, "اربعة": 4, "أربعة": 4, "اربع": 4, "أربع": 4, "خمسة": 5, "خمس": 5,
    "ستة": 6, "ست": 6, "سبعة": 7, "سبع": 7, "ثمانية": 8, "ثماني": 8, "ثمان": 8, "تسعة": 9, "تسع": 9,
    "عشرة": 10, "عشر": 10, "أحد عشر": 11, "احد عشر": 11, "اثنا عشر": 12, "اثني عشر": 12,
    "عشرون": 20, "عشرين": 20, "ثلاثون": 30, "ثلاثين": 30, "أربعون": 40, "اربعين": 40, "أربعين": 40,
    "خمسون": 50, "خمسين": 50, "ستون": 60, "ستين": 60, "سبعون": 70, "سبعين": 70,
    "ثمانون": 80, "ثمانين": 80, "تسعون": 90, "تسعين": 90, "مئة": 100, "مائة": 100,
}

OPERATORS = [
    # (regex, symbol) — longest phrases first
    (r"multiplied by|times|×|\*|ضرب|مضروب في|في", "*"),
    (r"divided by|over|÷|/|تقسيم|مقسوم على|على", "/"),
    (r"plus|add|and|\+|زائد|زائدا|و", "+"),
    (r"minus|take away|subtract|less|−|-|ناقص|ناقصا|اطرح", "-"),
]


class MathError(Exception):
    pass


@dataclass
class Evaluation:
    expression: str      # normalised, e.g. "7 + 5"
    value: Fraction

    @property
    def answer_text(self) -> str:
        return format_number(self.value)


def format_number(v: Fraction) -> str:
    if v.denominator == 1:
        return str(v.numerator)
    as_float = float(v)
    if round(as_float, 2) == as_float or v.denominator in (2, 4, 5, 10, 20, 25, 50, 100):
        return f"{as_float:g}"
    return f"{v.numerator}/{v.denominator}"


def _words_to_numbers_en(text: str) -> str:
    tokens = text.split()
    out: list[str] = []
    i = 0
    while i < len(tokens):
        t = tokens[i].strip("?,.!")
        if t in EN_TENS:
            n = EN_TENS[t]
            nxt = tokens[i + 1].strip("?,.!") if i + 1 < len(tokens) else ""
            if nxt in EN_UNITS and EN_UNITS[nxt] < 10:
                n += EN_UNITS[nxt]
                i += 1
            out.append(str(n))
        elif "-" in t and t.split("-", 1)[0] in EN_TENS and t.split("-", 1)[1] in EN_UNITS:
            a, b = t.split("-", 1)
            out.append(str(EN_TENS[a] + EN_UNITS[b]))
        elif t in EN_UNITS:
            out.append(str(EN_UNITS[t]))
        elif t in ("hundred", "a hundred"):
            if out and out[-1].isdigit():
                out[-1] = str(int(out[-1]) * 100)
            else:
                out.append("100")
        else:
            out.append(tokens[i])
        i += 1
    return " ".join(out)


def _words_to_numbers_ar(text: str) -> str:
    # Compound forms first ("خمسة وعشرون" = 25).
    for unit_word, unit in sorted(AR_NUMBERS.items(), key=lambda kv: -len(kv[0])):
        if unit >= 10:
            continue
        for tens_word, tens in AR_NUMBERS.items():
            if tens in (20, 30, 40, 50, 60, 70, 80, 90):
                text = text.replace(f"{unit_word} و{tens_word}", str(unit + tens))
    for word, n in sorted(AR_NUMBERS.items(), key=lambda kv: -len(kv[0])):
        text = re.sub(rf"(?<![\w]){re.escape(word)}(?![\w])", str(n), text)
    return text


def normalise(text: str) -> str:
    t = text.translate(ARABIC_INDIC).lower()
    t = t.replace("،", ",").replace("؟", "?")
    t = _words_to_numbers_en(t)
    t = _words_to_numbers_ar(t)
    return t


_TOKEN = re.compile(r"\s*(\d+(?:\.\d+)?|[+\-*/()])")


def _tokenise_math(fragment: str) -> list[str]:
    tokens: list[str] = []
    pos = 0
    while pos < len(fragment):
        m = _TOKEN.match(fragment, pos)
        if not m:
            raise MathError("unexpected input")
        tokens.append(m.group(1))
        pos = m.end()
    return tokens


def _parse(tokens: list[str]) -> Fraction:
    """Recursive-descent parser: expr := term (('+'|'-') term)* ; term := factor (('*'|'/') factor)*"""
    pos = 0

    def peek() -> str | None:
        return tokens[pos] if pos < len(tokens) else None

    def take() -> str:
        nonlocal pos
        tok = tokens[pos]
        pos += 1
        return tok

    def factor() -> Fraction:
        tok = peek()
        if tok is None:
            raise MathError("incomplete")
        if tok == "(":
            take()
            v = expr()
            if peek() != ")":
                raise MathError("unbalanced")
            take()
            return v
        if tok == "-":
            take()
            return -factor()
        take()
        if not re.fullmatch(r"\d+(?:\.\d+)?", tok):
            raise MathError("expected number")
        if len(tok) > 9:
            raise MathError("number too large")
        return Fraction(tok)

    def term() -> Fraction:
        v = factor()
        while peek() in ("*", "/"):
            op = take()
            rhs = factor()
            if op == "/":
                if rhs == 0:
                    raise MathError("division by zero")
                v = v / rhs
            else:
                v = v * rhs
        return v

    def expr() -> Fraction:
        v = term()
        while peek() in ("+", "-"):
            op = take()
            v = v + term() if op == "+" else v - term()
        return v

    value = expr()
    if pos != len(tokens):
        raise MathError("trailing input")
    return value


def find_arithmetic(text: str) -> Evaluation | None:
    """Return the arithmetic question in `text`, evaluated exactly, or None.

    Requires at least two numbers and one operator so "I am 6" is not treated as math.
    """
    t = normalise(text)
    for pattern, sym in OPERATORS:
        # Word operators need word boundaries; symbols do not.
        if pattern[0].isalpha() or "؀" <= pattern[0] <= "ۿ":
            t = re.sub(rf"(?<=[\s\d])(?:{pattern})(?=[\s\d])", f" {sym} ", t)
        else:
            t = re.sub(rf"(?:{pattern})", f" {sym} ", t)
    # Pull out the longest run that looks like an arithmetic expression.
    candidates = re.findall(r"[\d(][\d\s+\-*/().]*[\d)]", t)
    best: Evaluation | None = None
    for cand in candidates:
        nums = re.findall(r"\d+(?:\.\d+)?", cand)
        ops = re.findall(r"[+\-*/]", cand)
        if len(nums) < 2 or not ops:
            continue
        try:
            tokens = _tokenise_math(cand)
            value = _parse(tokens)
        except MathError:
            continue
        expr = " ".join(tokens).replace("*", "×").replace("/", "÷").replace("( ", "(").replace(" )", ")")
        if best is None or len(expr) > len(best.expression):
            best = Evaluation(expression=expr, value=value)
    return best


def extract_number(text: str) -> Fraction | None:
    """The single number a child answered with ("twelve", "١٢", "it's 12!")."""
    t = normalise(text)
    nums = re.findall(r"-?\d+(?:\.\d+)?", t)
    if len(nums) != 1:
        return None if len(nums) == 0 else Fraction(nums[-1])
    return Fraction(nums[0])


_CLAIM = re.compile(r"(\d+)\s*([+\-×x*÷/])\s*(\d+)\s*=\s*(\d+(?:\.\d+)?)")


def correct_claims(text: str) -> tuple[str, int]:
    """Find "a op b = c" statements in generated text and fix any wrong results.

    Returns (corrected_text, number_of_corrections). Language models can make arithmetic
    slips; a children's tutor must not repeat them.
    """
    fixes = 0

    def repl(m: re.Match[str]) -> str:
        nonlocal fixes
        a, op, b, claimed = Fraction(m.group(1)), m.group(2), Fraction(m.group(3)), m.group(4)
        try:
            if op == "+":
                v = a + b
            elif op == "-":
                v = a - b
            elif op in "×x*":
                v = a * b
            else:
                if b == 0:
                    return m.group(0)
                v = a / b
        except ArithmeticError:
            return m.group(0)
        if Fraction(claimed) == v:
            return m.group(0)
        fixes += 1
        return f"{m.group(1)} {op} {m.group(3)} = {format_number(v)}"

    return _CLAIM.sub(repl, text.translate(ARABIC_INDIC)), fixes
