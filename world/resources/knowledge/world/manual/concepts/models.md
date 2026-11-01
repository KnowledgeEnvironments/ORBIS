A Model is what a Principal thinks with. It is an Object: an endpoint, the model ids it offers, its prices and its status.

**Choosing.** A person chooses, for an agent or for one conversation, with the Model control beside the input. An agent never chooses for itself, because a Model spends someone's money or plan. Under the hood this is `model.choose`, and `model.chosen` says what a wake would use right now.

**When a Model is down.** A person can set a fallback. A Model that is unreachable, signed out or at its limit is passed over, the fallback answers, and `loop/model-note` says which one is standing in and why. If nothing is set, the wake fails with the reason.

**A ChatGPT plan.** A person can sign in with ChatGPT from a desktop client, and a Model appears that uses their plan instead of an API key. Its tokens are kept where no shell can read them. The plan has no number to show: the client says `Using ChatGPT plan` and links to ChatGPT's usage settings.
