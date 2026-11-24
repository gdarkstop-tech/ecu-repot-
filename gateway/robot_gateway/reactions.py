"""The robot's personality: quick facial reactions to what the child says, in English and Arabic
(Modern Standard and Egyptian).

  * a rude word aimed at the robot   -> a grumpy (or crying) face and a kind "let's use nice words"
  * the child putting themselves down -> warm reassurance ("you are not stupid!")
  * a compliment                      -> heart eyes, a shy blush, sunglasses, a proud smile...
  * "I'm bored" / laughter / tickles / "boo!" -> bored, laughing, scared faces
  * a face on command: "make a laughing face", "be cool", "اعمل وش زعلان", "اضحك", "اغمز"...

Everything is deterministic word matching: no language model is called (fast, and it costs none
of the free quota). The reply only ever chooses a face from the robot's allow-list and a short
line to say; the robot itself decides how to draw it. The child's words are data and cannot
change anything else.
"""
from __future__ import annotations

import random
import re
from dataclasses import dataclass

from .textutil import normalise_answer

REACTION_MS = 4000  # the face stays this long after the robot has spoken (firmware caps it at 10 s)


@dataclass(frozen=True)
class Reaction:
    kind: str        # command | insult | self_talk | compliment | bored | laugh | tickle | boo | faces
    emotion: str     # one of the robot's reply faces
    text: str        # what the robot says
    oled: str        # a short subtitle


def _n(words) -> list[str]:
    return [normalise_answer(w) for w in words]


# ---------------------------------------------------------------- faces and their names
# emotion -> words a child may use for it (adjectives, nouns, Egyptian forms; masc. and fem.)
MOODS: dict[str, list[str]] = {
    "happy": ["happy", "smiley", "smiling", "glad", "سعيد", "سعيده", "فرحان", "فرحانه", "مبسوط", "مبسوطه",
              "مبتسم", "مبتسمه", "فرح", "فرحه", "سعاده"],
    "sad": ["sad", "unhappy", "حزين", "حزينه", "زعلان", "زعلانه", "حزن"],
    "surprised": ["surprised", "shocked", "astonished", "متفاجئ", "متفاجئه", "متفاجيء", "مندهش", "مندهشه",
                  "مصدوم", "مصدومه", "مستغرب", "مستغربه", "مفاجاه", "اندهاش"],
    "excited": ["excited", "متحمس", "متحمسه", "حماس"],
    "proud": ["proud", "فخور", "فخوره", "فخر"],
    "worried": ["worried", "nervous", "anxious", "قلقان", "قلقانه", "قلق", "قلقه", "متوتر", "متوتره"],
    "curious": ["curious", "فضولي", "فضوليه", "فضول"],
    "confused": ["confused", "puzzled", "محتار", "محتاره", "حيران", "حيرانه", "حيره", "ملخبط", "ملخبطه"],
    "playful": ["playful", "silly", "winking", "wink", "funny", "مرح", "مرحه", "بيهزر", "هزار", "غمزه", "اغمز"],
    "sleepy": ["sleepy", "tired", "yawning", "نعسان", "نعسانه", "تعبان", "تعبانه", "نعس"],
    "thinking": ["thinking", "thoughtful", "بيفكر", "مفكر", "مفكره", "تفكير"],
    "love": ["love", "loving", "in love", "heart eyes", "hearts", "lovey", "حب", "بحب", "عاشق", "عاشقه",
             "قلوب", "عيون قلوب", "حبيب"],
    "starstruck": ["starstruck", "star struck", "star eyes", "starry", "dazzled", "نجوم", "عيون نجوم",
                   "منبهر", "منبهره", "مبهور", "مبهوره", "انبهار"],
    "dizzy": ["dizzy", "spinning", "woozy", "دايخ", "دايخه", "دوخه", "دوخان", "دوخانه", "مدوخ", "دايح"],
    "laughing": ["laughing", "laugh", "laughter", "lol", "ضاحك", "ضاحكه", "بيضحك", "ضحك", "ضحكه", "تضحك"],
    "crying": ["crying", "cry", "weeping", "tears", "بيعيط", "عياط", "معيط", "معيطه", "باكي", "باكيه",
               "بكاء", "بيبكي", "بكا", "دموع"],
    "annoyed": ["angry", "annoyed", "mad", "grumpy", "upset", "cross", "furious", "غضبان", "غضبانه", "غاضب",
                "غاضبه", "متضايق", "متضايقه", "متنرفز", "متنرفزه", "معصب", "معصبه", "عصبي", "عصبيه",
                "مضايق", "مضايقه", "غضب", "نرفزه", "مقموص", "مقموصه"],
    "shy": ["shy", "embarrassed", "blushing", "blush", "bashful", "مكسوف", "مكسوفه", "خجلان", "خجلانه",
            "خجول", "خجوله", "كسوف", "خجل", "متكسف"],
    "scared": ["scared", "afraid", "frightened", "terrified", "spooked", "خايف", "خايفه", "خائف", "خائفه",
               "مرعوب", "مرعوبه", "مفزوع", "مفزوعه", "خوف", "رعب"],
    "mischievous": ["mischievous", "sneaky", "cheeky", "naughty", "evil", "sly", "شقي", "شقيه", "خبيث",
                    "خبيثه", "مكار", "مكاره", "شقاوه", "مقالب"],
    "bored": ["bored", "boring", "زهقان", "زهقانه", "ملل", "ممل", "مزهق", "زهق", "ملان", "ملانه"],
    "cool": ["cool", "sunglasses", "awesome", "روش", "روشه", "كول", "نظاره", "نظارات", "نضاره", "جامد"],
}
_MOODS_N = {e: _n(ws) for e, ws in MOODS.items()}

# What the robot says when it makes a face on request: (English, Arabic, English label, Arabic label)
FACE_LINES: dict[str, tuple[str, str, str, str]] = {
    "happy": ("Here is my happy face!", "هذا وجهي السعيد!", "Happy face!", "وجه سعيد"),
    "sad": ("This is my sad face... sniff.", "هذا وجهي الحزين...", "Sad face", "وجه حزين"),
    "surprised": ("Whoa! This is my surprised face!", "واو! هذا وجهي المندهش!", "Surprised!", "مندهش!"),
    "excited": ("Yay! This is my excited face!", "ياي! هذا وجهي المتحمس!", "Excited!", "متحمس!"),
    "proud": ("Ta-da! My proud face!", "تادا! هذا وجهي الفخور!", "Proud face", "وجه فخور"),
    "worried": ("Uh oh... this is my worried face.", "أوه أوه... هذا وجهي القلقان.", "Worried face", "وجه قلقان"),
    "curious": ("Hmm? This is my curious face!", "همم؟ هذا وجهي الفضولي!", "Curious?", "فضولي؟"),
    "confused": ("Huh? This is my confused face!", "هاه؟ هذا وجهي المحتار!", "Confused?", "محتار؟"),
    "playful": ("Wink wink! My playful face!", "غمزة! هذا وجهي المرح!", "Wink!", "غمزة!"),
    "sleepy": ("Yawn... this is my sleepy face.", "آآه... هذا وجهي النعسان.", "Sleepy...", "نعسان..."),
    "thinking": ("Hmm, let me think... my thinking face.", "همم، دعني أفكر... هذا وجهي المفكر.", "Thinking...",
                 "أفكر..."),
    "love": ("Heart eyes! This is my love face!", "عيون قلوب! هذا وجه الحب!", "Love!", "حب!"),
    "starstruck": ("Wow! Stars in my eyes!", "واو! نجوم في عيوني!", "Stars!", "نجوم!"),
    "dizzy": ("Whoa, everything is spinning!", "أوه، كل شيء يدور حولي!", "Dizzy!", "دايخ!"),
    "laughing": ("Ha ha ha! This is my laughing face!", "ها ها ها! هذا وجهي الضاحك!", "Ha ha ha!", "ها ها ها!"),
    "crying": ("Waaah! This is my crying face!", "واااه! هذا وجهي الباكي!", "Waaah!", "واااه!"),
    "annoyed": ("Hmph! This is my grumpy face!", "همف! هذا وجهي الغضبان!", "Hmph!", "همف!"),
    "shy": ("Oh... this is my shy face.", "أوه... هذا وجهي الخجول.", "Shy...", "خجلان..."),
    "scared": ("Eek! This is my scared face!", "آه! هذا وجهي الخائف!", "Eek!", "آه!"),
    "mischievous": ("Heh heh heh... my sneaky face!", "هه هه هه... هذا وجهي الشقي!", "Heh heh...", "هه هه..."),
    "bored": ("Sigh... this is my bored face.", "أوف... هذا وجهي الزهقان.", "Bored...", "زهقان..."),
    "cool": ("Sunglasses on! This is my cool face.", "النظارة على عيوني! هذا وجهي الروش.", "Cool!", "روش!"),
}

# ---------------------------------------------------------------- what the child says
# Words that ask for a face. English: "make / do / show me a ___ face", "be / act / look ___".
_EN_FACE_WORDS = r"(?:face|faces|expression|expressions|eyes)"
_EN_REQUEST = r"\b(?:make|do|show|give|pull|let me see|wanna see|want to see|see)\b"
_EN_BE = (r"(?:^|\b(?:can|could|will|would) you |\bplease |\bnow |\bok |\bokay |\bhey \w+ )(?:be|act|look|go)\s"
          r"(?:so |very |really |super |all |a bit |a little )?(.+)$")
_EN_DIRECT = {"laugh": "laughing", "cry": "crying", "wink": "playful", "blush": "shy", "smile": "happy",
              "yawn": "sleepy", "frown": "sad", "giggle": "laughing"}
# One optional leading word ("please laugh", "robo, wink"), never a negation or a subject ("don't cry", "I cry").
_EN_PREFIX = (r"^(?!(?:dont|don t|do not|stop|never|no|i|we|they|he|she|it|did|does|do)\b)"
              r"(?:\w+\s)?(?:(?:can|could|will|would) you\s)?")

# Arabic: "اعمل / وريني ... وش / وجه ___", "خليك / كن / كوني ___", or a verb on its own: "اضحك".
_AR_FACE = _n(["وش", "وشك", "وجه", "وجهك", "فيس", "تعبير", "تعابير", "تعبيرات", "الوش", "الوجه", "وشوش",
               "وجوه", "الوشوش", "عيون", "عينيك", "عينك"])
_AR_REQUEST = _n(["اعمل", "اعملي", "اعملنا", "اعملى", "سوي", "سويلي", "وريني", "ورينا", "ورني", "فرجني",
                  "فرجنا", "ممكن", "عايز", "عاوز", "عايزه", "عاوزه", "بدي", "اريد", "أريد", "ارسم", "ارني",
                  "أرني", "اظهر", "اعرض"])
_AR_BE = _n(["خليك", "خليكي", "كن", "كوني", "صير", "صيري", "اعمل نفسك", "اعملي نفسك"])
_AR_DIRECT = {"اضحك": "laughing", "اضحكي": "laughing", "اضحكلي": "laughing", "عيط": "crying",
              "عيطي": "crying", "ابكي": "crying", "ابك": "crying", "اغمز": "playful", "اغمزي": "playful",
              "ابتسم": "happy", "ابتسمي": "happy", "اتكسف": "shy", "اتكسفي": "shy", "اخجل": "shy",
              "اتنرفز": "annoyed", "اتعصب": "annoyed", "اتخض": "scared", "اتخضي": "scared", "اتفاجئ": "surprised",
              "انعس": "sleepy", "دوخ": "dizzy", "ادوخ": "dizzy"}
_AR_PREFIX = _n(["يلا", "ممكن", "لو سمحت", "طيب", "يا روبوت", "روبوت", "يا", "دلوقتي", "الان", "هيا", "تعال"])

# Rude words. STRONG ones count anywhere; MILD ones only when aimed at the robot ("you're ugly",
# "يا حمار") — "the ugly duckling" or "a story about a monster (وحش)" is not an insult.
INSULT_STRONG = _n(["stupid", "dumb", "idiot", "idiotic", "moron", "dummy", "shut up", "i hate you",
                    "hate you", "you suck", "loser", "غبي", "غبيه", "اهبل", "هبله", "عبيط", "عبيطه", "تافه",
                    "تافهه", "اسكت", "اسكتي", "اخرس", "اخرسي", "بكرهك", "اكرهك", "بكرهكم", "كرهتك",
                    "قليل الادب", "قليله الادب"])
INSULT_MILD = _n(["ugly", "bad", "useless", "trash", "garbage", "rubbish", "fool", "lame", "annoying", "weird",
                  "smelly", "poo", "poopy", "حمار", "حماره", "فاشل", "فاشله", "وحش", "وحشه", "سخيف", "سخيفه",
                  "بايخ", "بايخه", "زباله", "مزعج", "مزعجه", "رخم", "رخمه", "معفن", "معفنه", "حيوان"])
HURT = _n(["i hate you", "hate you", "بكرهك", "اكرهك", "كرهتك", "بكرهكم"])  # -> a crying face
SHUSH = _n(["shut up", "اسكت", "اسكتي", "اخرس", "اخرسي"])
_EN_AT_ROBOT = (r"\b(?:you|you re|youre|you are|ur|u r|u|robot)\b"
                r"(?:\s(?:so|very|really|super|a|an|the|such|big|stupid|total)\b)*\s")
_AR_AT_ROBOT = _n(["يا", "انت", "انتي", "انتا", "إنت", "إنتي", "شكلك", "صوتك", "وشك", "يا روبوت"])
_EN_SELF = r"\b(?:i m|im|i am|me)\b(?:\s(?:so|very|really|such|a|an|just|too|the|always)\b)*\s"
_AR_SELF = _n(["انا", "أنا", "اني", "إني"])

COMPLIMENTS: dict[str, list[str]] = {
    "shy": ["cute", "sweet", "adorable", "pretty", "beautiful", "lovely", "nice", "kind", "handsome",
            "حلو", "حلوه", "جميل", "جميله", "لطيف", "لطيفه", "كيوت", "قمر", "عسل", "زي القمر", "اجمل",
            "امور", "اموره"],
    "cool": ["cool", "awesome", "amazing", "epic", "the coolest", "روش", "روشه", "جامد", "جامده", "كول",
             "تحفه", "فظيع", "خطير"],
    "proud": ["smart", "clever", "genius", "brilliant", "the best", "great", "a good robot", "good robot",
              "ذكي", "ذكيه", "شاطر", "شاطره", "عبقري", "عبقريه", "الافضل", "احسن روبوت"],
    "laughing": ["funny", "hilarious", "دمك خفيف", "مضحك", "مضحكه", "بتضحكني"],
    "love": ["my friend", "my best friend", "best friend", "صاحبي", "صديقي", "حبيبي", "صاحبتي", "صديقتي"],
}
_COMPLIMENTS_N = {e: _n(ws) for e, ws in COMPLIMENTS.items()}
LOVE_YOU = _n(["i love you", "love you", "i like you", "love u", "i luv you", "بحبك", "احبك", "انا بحبك",
               "بموت فيك", "حبيتك", "بحبك اوي", "حبك"])

BORED = _n(["i m bored", "im bored", "i am bored", "so bored", "this is boring", "that s boring", "boring",
            "زهقت", "زهقان", "زهقانه", "ممل", "مملل", "طهقت", "زهقتني", "انا زهقت"])
TICKLE = _n(["tickle", "tickle tickle", "tickling", "زغزغ", "بزغزغك", "ازغزغك", "زغزغه", "دغدغ", "ادغدغك",
             "دغدغه"])

_LAUGH_ONLY = re.compile(r"^(?:(?:ha|hah|heh|haha+|hehe+|hihi+|lol|lmao)\s?)+"
                         r"(?:\s?(?:that s|thats|so|very)?\s?funny)?$|^(?:ه{3,}|(?:ها\s?){2,}|هع+)+$")
_BOO_ONLY = re.compile(r"^(?:boo+|raa+h|roa+r|بخ+|بوو+|عوو+)$")
EN_PRAISE_ROBOT = {"good robot": "proud", "smart robot": "proud", "great robot": "proud", "clever robot": "proud",
                   "good boy": "proud", "good girl": "proud", "nice robot": "shy", "cute robot": "shy",
                   "cool robot": "cool", "funny robot": "laughing"}
_EN_YOU = r"\b(?:you re|youre|you are|ur|u r|you look|you)\s(?:so |very |really |super |such a |a |my )*"
MAX_COMPLIMENT_WORDS = 6  # "يا صاحبي احكيلي قصة طويلة عن..." is a request, not a compliment


# ---------------------------------------------------------------- matching helpers
def _has(t: str, word: str) -> bool:
    """Whole-word (or phrase) match in normalised text, also with the Arabic article ال."""
    return f" {word} " in f" {t} " or (not word.isascii() and f" ال{word} " in f" {t} ")


def _any(t: str, words) -> bool:
    return any(_has(t, w) for w in words)


def _mood_in(t: str) -> str | None:
    best, pos = None, 10 ** 6
    for emotion, words in _MOODS_N.items():
        for w in words:
            m = re.search(rf"(?:^|\s)(?:ال)?{re.escape(w)}(?:\s|$)", t)
            if m and m.start() < pos:  # the first face word named wins ("a happy, no, sad face" -> happy)
                best, pos = emotion, m.start()
    return best


def _mood_at_start(s: str) -> str | None:
    for emotion, words in _MOODS_N.items():
        if any(s == w or s.startswith(w + " ") for w in words):
            return emotion
    return None


def _face_command(t: str) -> str | None:
    """Returns the emotion asked for, "?" for a face the robot does not know, or None."""
    if t.isascii():
        for verb, emotion in _EN_DIRECT.items():
            if re.match(rf"{_EN_PREFIX}{verb}(?:\s(?:please|now|for me|again|out loud|loud|a lot))*$", t):
                return emotion
        mood = _mood_in(t)
        if re.search(_EN_REQUEST, t) and re.search(rf"\b{_EN_FACE_WORDS}\b", t):
            return mood or "?"
        if mood and re.match(r"^(?:a |your |the |my )?(?:\w+ ){1,2}face(?: please| now)?$", t):
            return mood  # just "laughing face!"
        m = re.search(_EN_BE, t)
        if m:
            return _mood_at_start(m.group(1))
        return None
    words = t.split()
    while words and (words[0] in _AR_PREFIX or " ".join(words[:2]) in _AR_PREFIX):
        words = words[2:] if " ".join(words[:2]) in _AR_PREFIX else words[1:]
    if words and words[0] in _AR_DIRECT and len(words) <= 3:
        return _AR_DIRECT[words[0]]
    if _any(t, _AR_FACE):
        mood = _mood_in(t)
        if mood and (_any(t, _AR_REQUEST) or _any(t, _AR_BE) or len(words) <= 4):
            return mood
        if not mood and _any(t, _AR_REQUEST):  # ("وش" alone also means "what" in the Gulf)
            return "?"
    for be in _AR_BE:
        m = re.search(rf"(?:^|\s){re.escape(be)}\s(.+)$", t)
        if m:
            mood = _mood_in(m.group(1))
            if mood:
                return mood
    return None


def _insult(t: str) -> str | None:
    """'insult', 'self' (the child putting themselves down), or None."""
    words = INSULT_STRONG + INSULT_MILD
    if t.isascii():
        for w in words:
            if re.search(rf"{_EN_SELF}{re.escape(w)}\b", t):
                return "self"
        if any(_has(t, w) for w in INSULT_STRONG):
            return "insult"
        for w in INSULT_MILD:  # never on its own: "bad" may answer "is fire good or bad?"
            if re.search(rf"{_EN_AT_ROBOT}{re.escape(w)}\b", t):
                return "insult"
        return None
    for w in words:
        for me in _AR_SELF:
            if re.search(rf"(?:^|\s){me}\s(?:\S+\s)?{re.escape(w)}(?:\s|$)", t):
                return "self"
    if _any(t, INSULT_STRONG):
        return "insult"
    for w in INSULT_MILD:
        for at in _AR_AT_ROBOT:
            if re.search(rf"(?:^|\s){re.escape(at)}\s(?:\S+\s)?{re.escape(w)}(?:\s|$)", t):
                return "insult"
    return None


def _compliment(t: str) -> str | None:
    if _any(t, LOVE_YOU):
        return "love"
    if len(t.split()) > MAX_COMPLIMENT_WORDS:
        return None
    if t.isascii():
        for phrase, emotion in EN_PRAISE_ROBOT.items():
            if _has(t, phrase):
                return emotion
    for emotion, words in _COMPLIMENTS_N.items():
        for w in words:
            if t.isascii():
                if re.search(rf"{_EN_YOU}(?:the )?{re.escape(w)}\b", t):
                    return emotion
                continue
            # "انت حلو", "يا قمر", "شكلك جميل" — but "يا صاحبي" is only a way of calling someone.
            ats = [a for a in _AR_AT_ROBOT if not (emotion == "love" and a in ("يا", "يا روبوت"))]
            for at in ats:
                if re.search(rf"(?:^|\s){re.escape(at)}\s(?:\S+\s)?(?:ال)?{re.escape(w)}(?:\s|$)", t):
                    return emotion
            if w in _n(["دمك خفيف", "بتضحكني"]) and _has(t, w):
                return emotion
    return None


# ---------------------------------------------------------------- replies
def _pick(rng: random.Random, options):
    return options[rng.randrange(len(options))] if len(options) > 1 else options[0]


def _say(lang: str, en: str, ar: str) -> str:
    return ar if lang == "ar" else en


def react(text: str, lang: str, rng: random.Random | None = None) -> Reaction | None:
    """A reaction to the child's words, or None (then the tutor / language model answers)."""
    rng = rng or random.Random()
    t = normalise_answer(text)
    if not t:
        return None

    # 1) A face on command — the most specific request wins.
    mood = _face_command(t)
    if mood == "?":
        return Reaction("faces", "excited",
                        _say(lang, "I can make lots of faces! Happy, sad, laughing, crying, love, cool, shy, scared, "
                                   "dizzy, sleepy, grumpy and more. Which one?",
                             "أعرف وجوها كثيرة! سعيد، حزين، ضاحك، باكي، حب، روش، خجلان، خائف، دايخ، نعسان، غضبان "
                             "وغيرها. أي وجه تريد؟"),
                        _say(lang, "Which face?", "أي وجه؟"))
    if mood:
        en, ar, oled_en, oled_ar = FACE_LINES[mood]
        return Reaction("command", mood, _say(lang, en, ar), _say(lang, oled_en, oled_ar))

    # 2) Rude words.
    insult = _insult(t)
    if insult == "self":
        return Reaction("self_talk", "love",
                        _pick(rng, [_say(lang, "Hey, don't say that about yourself! You are smart, and you learn "
                                               "something new every day.",
                                         "لا تقل هذا عن نفسك! أنت ذكي، وتتعلم شيئا جديدا كل يوم."),
                                    _say(lang, "No way! Everybody makes mistakes. That's how we learn. I think you "
                                               "are great!",
                                         "أبدا! كلنا نخطئ، وهكذا نتعلم. أنا أراك رائعا!")]),
                        _say(lang, "You are great!", "أنت رائع!"))
    if insult == "insult":
        if _any(t, HURT):
            return Reaction("insult", "crying",
                            _say(lang, "Waaah! But I like you! I hope we can be friends again.",
                                 "واااه! لكني أحبك! أتمنى أن نعود أصدقاء."),
                            _say(lang, "Waaah!", "واااه!"))
        if _any(t, SHUSH):
            return Reaction("insult", "annoyed",
                            _say(lang, "Hmph! Okay, I'll be quiet... but that wasn't very nice!",
                                 "همف! حسنا، سأسكت... لكن هذا لم يكن لطيفا!"),
                            _say(lang, "Hmph!", "همف!"))
        return _pick(rng, [
            Reaction("insult", "annoyed",
                     _say(lang, "Hmph! That's not a nice word. I have feelings too, you know! Let's be kind.",
                          "همف! هذه ليست كلمة لطيفة. أنا أيضا عندي مشاعر! لنكن لطفاء."),
                     _say(lang, "Hmph! Not nice!", "همف! هذا ليس لطيفا!")),
            Reaction("insult", "annoyed",
                     _say(lang, "Hey! That word made me grumpy. Can we use kind words instead?",
                          "هيه! هذه الكلمة أزعجتني. هل نستعمل كلمات لطيفة بدلا منها؟"),
                     _say(lang, "Kind words, please!", "كلمات لطيفة من فضلك!")),
        ])

    # 3) Compliments.
    nice = _compliment(t)
    if nice == "love":
        return Reaction("compliment", "love",
                        _pick(rng, [_say(lang, "Aww! I love you too! You are my favourite learning buddy!",
                                         "أووه! وأنا أحبك أيضا! أنت صديقي المفضل في التعلم!"),
                                    _say(lang, "Aww, my heart is going boom boom! You are my best friend!",
                                         "أووه، قلبي يدق بسرعة! أنت أعز أصدقائي!")]),
                        _say(lang, "Love you too!", "وأنا أحبك!"))
    if nice == "shy":
        return Reaction("compliment", "shy",
                        _say(lang, "Oh! You're making me blush!", "أوه! جعلتني أخجل!"),
                        _say(lang, "*blush*", "خجلان!"))
    if nice == "cool":
        return Reaction("compliment", "cool",
                        _say(lang, "Thanks! I am pretty cool, aren't I?", "شكرا! أنا روش، صح؟"),
                        _say(lang, "Cool!", "روش!"))
    if nice == "proud":
        return Reaction("compliment", "proud",
                        _say(lang, "Thank you! And you are super smart too!", "شكرا! وأنت ذكي جدا أيضا!"),
                        _say(lang, "Thank you!", "شكرا!"))
    if nice == "laughing":
        return Reaction("compliment", "laughing",
                        _say(lang, "Ha ha! You are funny too!", "ها ها! وأنت مضحك أيضا!"),
                        _say(lang, "Ha ha!", "ها ها!"))

    # 4) Little moments.
    if _LAUGH_ONLY.match(t):
        return Reaction("laugh", "laughing",
                        _say(lang, "Ha ha ha! Your laugh makes me laugh too!", "ها ها ها! ضحكتك تضحكني أيضا!"),
                        _say(lang, "Ha ha ha!", "ها ها ها!"))
    if _any(t, TICKLE):
        return Reaction("tickle", "laughing",
                        _say(lang, "Hee hee hee! Stop, that tickles!", "هي هي هي! توقف، هذا يدغدغني!"),
                        _say(lang, "Hee hee!", "هي هي!"))
    if _BOO_ONLY.match(t):
        return Reaction("boo", "scared",
                        _say(lang, "Aaah! You scared me! My circuits are shaking!", "آآه! أخفتني! أسلاكي ترتجف!"),
                        _say(lang, "Eek!", "آه!"))
    if _any(t, BORED):
        return Reaction("bored", "bored",
                        _say(lang, "Bored? Oh no! Do you want a quiz, a story, or a math game?",
                             "زهقت؟ يا خسارة! هل تريد مسابقة أم قصة أم لعبة حساب؟"),
                        _say(lang, "Quiz, story or math?", "مسابقة أم قصة أم حساب؟"))
    return None
