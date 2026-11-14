"""Curated, child-safe learning content in English and Arabic.

Stories and life lessons are written here (not generated) so they are reviewed once and
always safe. Extend the lists freely; keep sentences short — they are spoken aloud.
"""
from __future__ import annotations

EN_ALPHABET = [
    ("A", "Apple"), ("B", "Ball"), ("C", "Cat"), ("D", "Dog"), ("E", "Elephant"), ("F", "Fish"),
    ("G", "Grapes"), ("H", "Hat"), ("I", "Ice cream"), ("J", "Juice"), ("K", "Kite"), ("L", "Lion"),
    ("M", "Moon"), ("N", "Nest"), ("O", "Orange"), ("P", "Pencil"), ("Q", "Queen"), ("R", "Rabbit"),
    ("S", "Sun"), ("T", "Tree"), ("U", "Umbrella"), ("V", "Violin"), ("W", "Whale"), ("X", "Xylophone"),
    ("Y", "Yo-yo"), ("Z", "Zebra"),
]

# (letter, letter name, example word, English gloss)
AR_ALPHABET = [
    ("ا", "ألف", "أسد", "lion"), ("ب", "باء", "بطة", "duck"), ("ت", "تاء", "تفاحة", "apple"),
    ("ث", "ثاء", "ثعلب", "fox"), ("ج", "جيم", "جمل", "camel"), ("ح", "حاء", "حصان", "horse"),
    ("خ", "خاء", "خروف", "sheep"), ("د", "دال", "دب", "bear"), ("ذ", "ذال", "ذرة", "corn"),
    ("ر", "راء", "رمان", "pomegranate"), ("ز", "زاي", "زرافة", "giraffe"), ("س", "سين", "سمكة", "fish"),
    ("ش", "شين", "شمس", "sun"), ("ص", "صاد", "صقر", "falcon"), ("ض", "ضاد", "ضفدع", "frog"),
    ("ط", "طاء", "طائرة", "airplane"), ("ظ", "ظاء", "ظرف", "envelope"), ("ع", "عين", "عصفور", "sparrow"),
    ("غ", "غين", "غزال", "gazelle"), ("ف", "فاء", "فيل", "elephant"), ("ق", "قاف", "قطة", "cat"),
    ("ك", "كاف", "كتاب", "book"), ("ل", "لام", "ليمون", "lemon"), ("م", "ميم", "موز", "banana"),
    ("ن", "نون", "نحلة", "bee"), ("ه", "هاء", "هدية", "gift"), ("و", "واو", "وردة", "rose"),
    ("ي", "ياء", "يد", "hand"),
]

# (English, Arabic) vocabulary pairs, easiest first.
VOCAB = [
    ("cat", "قطة"), ("dog", "كلب"), ("sun", "شمس"), ("moon", "قمر"), ("apple", "تفاحة"),
    ("water", "ماء"), ("book", "كتاب"), ("house", "بيت"), ("tree", "شجرة"), ("bird", "عصفور"),
    ("fish", "سمكة"), ("car", "سيارة"), ("ball", "كرة"), ("milk", "حليب"), ("bread", "خبز"),
    ("door", "باب"), ("flower", "وردة"), ("hand", "يد"), ("eye", "عين"), ("red", "أحمر"),
    ("blue", "أزرق"), ("green", "أخضر"), ("mother", "أم"), ("father", "أب"), ("friend", "صديق"),
    ("school", "مدرسة"), ("teacher", "معلم"), ("star", "نجمة"), ("rain", "مطر"), ("happy", "سعيد"),
]

EN_NUMBER_WORDS = [
    "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
    "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen",
]
EN_TENS_WORDS = {2: "twenty", 3: "thirty", 4: "forty", 5: "fifty", 6: "sixty", 7: "seventy", 8: "eighty", 9: "ninety"}


def number_word_en(n: int) -> str:
    if n == 100:
        return "one hundred"
    if n < 20:
        return EN_NUMBER_WORDS[n]
    t, o = divmod(n, 10)
    return EN_TENS_WORDS[t] + ("" if o == 0 else "-" + EN_NUMBER_WORDS[o])


STORIES = {
    "en": [
        "Once upon a time, a little turtle named Tim wanted to see the sea. The road was long, "
        "but Tim took one small step at a time. At sunset he finally heard the waves. "
        "Tim smiled and said: slow steps still reach far.",
        "Lina had one orange and her friend Sami had none. Lina cut her orange in two and shared it. "
        "Sami laughed and shared his storybook. That afternoon they both had a snack and a story.",
        "A small cloud was sad because it was not big like the others. Then it rained on a thirsty "
        "flower. The flower opened its petals and said thank you. Even small helpers matter.",
    ],
    "ar": [
        "كان يا ما كان، سلحفاة صغيرة اسمها سلمى أرادت أن ترى البحر. كان الطريق طويلا، لكنها مشت "
        "خطوة صغيرة بعد خطوة. وعند الغروب سمعت صوت الأمواج. قالت سلمى: الخطوات الصغيرة توصلنا بعيدا.",
        "كان مع ليلى برتقالة واحدة، ولم يكن مع صديقها سامي شيء. قسمت ليلى البرتقالة نصفين وأعطته نصفا. "
        "فرح سامي وشاركها كتاب قصصه. وفي ذلك اليوم أكل الاثنان وقرآ معا.",
        "كانت غيمة صغيرة حزينة لأنها ليست كبيرة مثل الغيوم الأخرى. ثم أمطرت على وردة عطشى. "
        "فتحت الوردة أوراقها وقالت: شكرا لك. حتى المساعد الصغير مهم.",
    ],
}

WISDOM = {
    "en": [
        "Saying please and thank you makes everyone feel good.",
        "When you make a mistake, say sorry and try again. That is how we learn.",
        "Drink water and wash your hands. A healthy body helps a happy mind.",
        "Be kind to animals. They have feelings too.",
        "If you feel sad or scared, talk to a grown-up you trust.",
        "Practice a little every day, and you will get better and better.",
    ],
    "ar": [
        "قول من فضلك وشكرا يجعل الجميع سعداء.",
        "عندما نخطئ نعتذر ونحاول مرة أخرى. هكذا نتعلم.",
        "اشرب الماء واغسل يديك. الجسم السليم يساعد على العقل السعيد.",
        "كن لطيفا مع الحيوانات، فهي تشعر مثلنا.",
        "إذا شعرت بالحزن أو الخوف، تحدث إلى شخص كبير تثق به.",
        "تدرّب قليلا كل يوم، وستصبح أفضل وأفضل.",
    ],
}

# General-knowledge quiz items: (question_en, question_ar, accepted answers)
QUIZ = [
    ("How many legs does a cat have?", "كم رجلا للقطة؟", {"4", "four", "أربع", "أربعة", "اربعة"}),
    ("What color is the sky on a sunny day?", "ما لون السماء في يوم مشمس؟", {"blue", "أزرق", "ازرق", "زرقاء"}),
    ("Which letter comes after B?", "أي حرف يأتي بعد الألف والباء؟", {"c", "ت", "تاء"}),
    ("How many days are in a week?", "كم يوما في الأسبوع؟", {"7", "seven", "سبعة", "سبع"}),
    ("What do bees make?", "ماذا يصنع النحل؟", {"honey", "عسل", "العسل"}),
    ("Is the sun hot or cold?", "هل الشمس حارة أم باردة؟", {"hot", "حارة", "حار", "ساخنة"}),
]

# Small answer bank so demo mode can still hold a simple conversation.
DEMO_QA = [
    ({"your name", "who are you", "اسمك", "من انت", "من أنت"},
     "I am {name}, your learning robot! I can teach letters, numbers, math, words and stories.",
     "أنا {name}، روبوت التعلم! أستطيع أن أعلمك الحروف والأرقام والحساب والكلمات والقصص."),
    ({"how are you", "كيف حالك"},
     "I am happy and ready to learn with you!",
     "أنا سعيد ومستعد لأتعلم معك!"),
    ({"sky blue", "السماء زرقاء"},
     "Sunlight has many colors. The air scatters blue light the most, so the sky looks blue.",
     "ضوء الشمس فيه ألوان كثيرة، والهواء ينشر اللون الأزرق أكثر، لذلك نرى السماء زرقاء."),
    ({"hello", "hi", "مرحبا", "اهلا", "أهلا", "السلام عليكم"},
     "Hello, friend! Do you want to learn letters, numbers, math, or hear a story?",
     "مرحبا يا صديقي! هل تريد أن نتعلم الحروف أو الأرقام أو الحساب أو نسمع قصة؟"),
]
