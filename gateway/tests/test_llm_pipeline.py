from conftest import AUTH, make_client

from robot_gateway.providers.base import LlmResult, ProviderError


class FakeLLM:
    name = "fake"

    def __init__(self, text="", refused=False, error=None):
        self.text, self.refused, self.error = text, refused, error
        self.calls = []

    def reply(self, system, history, user_text):
        self.calls.append((system, list(history), user_text))
        if self.error:
            raise self.error
        return LlmResult(text=self.text, refused=self.refused, model="fake")


def ask(llm, text="why do cats purr?"):
    c = make_client(llm=llm)
    return c.post("/v1/turns/text", headers=AUTH, json={"text": text}).json()


def test_llm_answer_is_cleaned_for_speech():
    b = ask(FakeLLM("**Cats purr** when they are happy! 😺 See https://x.y"))
    assert b["reply_text"] == "Cats purr when they are happy! See" or "Cats purr" in b["reply_text"]
    assert "*" not in b["reply_text"] and "http" not in b["reply_text"]


def test_wrong_arithmetic_from_model_is_corrected():
    b = ask(FakeLLM("Sure! 5 + 5 = 11. Nice!"), "is five a big number?")
    assert "5 + 5 = 10" in b["reply_text"] and "arithmetic_corrected_1" in b["warnings"]


def test_unsafe_model_output_replaced():
    b = ask(FakeLLM("You could use a knife for that."))
    assert "knife" not in b["reply_text"] and "output_violence" in b["warnings"]


def test_refusal_and_errors_fall_back_gracefully():
    b = ask(FakeLLM(refused=True))
    assert "llm_refused" in b["warnings"] and b["reply_text"]
    b = ask(FakeLLM(error=ProviderError("llm_unreachable")))
    assert "llm_unreachable" in b["warnings"] and b["emotion"] == "sad"


def test_child_words_are_delimited_and_personal_data_redacted():
    llm = FakeLLM("Okay!")
    ask(llm, "my email is kid@example.com, why is grass green?")
    # personal info is caught by the input check before reaching the model at all
    assert llm.calls == []
    llm2 = FakeLLM("Grass is green because of chlorophyll.")
    ask(llm2, "why is grass green?")
    system, history, user = llm2.calls[0]
    assert "«why is grass green?»" in user
    assert "Never ask for or repeat personal information" in system


def test_history_is_kept_between_turns():
    llm = FakeLLM("Interesting!")
    c = make_client(llm=llm)
    c.post("/v1/turns/text", headers=AUTH, json={"text": "why do birds sing?"})
    c.post("/v1/turns/text", headers=AUTH, json={"text": "why do frogs jump?"})
    assert len(llm.calls[1][1]) == 2  # previous user + assistant
