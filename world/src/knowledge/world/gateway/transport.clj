(ns knowledge.world.gateway.transport
  "What a message gateway does, one interface whatever is on the other side. conn is
   {:kind :url :password}. A transport never logs and never returns its secret: errors come back as
   a status word, because an exception from an HTTP client can quote the URL, and some services carry
   the password there.")

(defmulti ping
  "Can it be reached? :ok | :unreachable | :unauthenticated."
  :kind)

(defmulti fetch
  "Messages since {:after ms}, oldest first: {:status word :messages [{:id :handle :text :at :from-me? :chat}]}."
  (fn [conn _] (:kind conn)))

(defmulti push
  "One outbound text, {:handle :chat :text}: {:status word}."
  (fn [conn _] (:kind conn)))
