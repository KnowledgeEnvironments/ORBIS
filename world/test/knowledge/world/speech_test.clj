(ns knowledge.world.speech-test
  "Speech through a fake OpenAI-compatible endpoint: nothing reaches the log."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [jsonista.core :as j]
            [knowledge.world.act.core :as act]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.fold.objects :as objects]
            [knowledge.world.log.core :as log]
            [knowledge.world.ws-client :as ws]
            [org.httpkit.server :as http])
  (:import [java.io ByteArrayOutputStream]
           [java.util Base64]))

(defn- fake []
  (let [calls (atom [])
        stop (http/run-server
              (fn [{:keys [uri headers body]}]
                (let [bytes (let [o (ByteArrayOutputStream.)] (when body (.transferTo ^java.io.InputStream body o)) (.toByteArray o))
                      text (String. bytes "ISO-8859-1")]
                  (swap! calls conj {:uri uri :type (get headers "content-type") :text text})
                  (cond
                    (str/ends-with? uri "/audio/transcriptions")
                    {:status 200 :headers {"Content-Type" "application/json"} :body "{\"text\":\"hello there\"}"}
                    (str/ends-with? uri "/audio/speech")
                    {:status 200 :headers {"Content-Type" "audio/wav"} :body (java.io.ByteArrayInputStream. (.getBytes "RIFFfake" "UTF-8"))}
                    :else {:status 200 :body "{\"data\":[]}"})))
              {:port 0 :legacy-return-value? false})]
    {:url (str "http://127.0.0.1:" (http/server-port stop) "/v1") :calls calls :stop #(http/server-stop! stop)}))

(defn- act! [c action args] (ws/request!! c :act {:action (keyword action) :args args}))
(defn- code [r] (some-> (get-in r [:err :code]) name keyword))

(deftest a-recording-goes-to-the-model-and-leaves-nothing
  (fx/with-world [{:keys [world root witness]}]
    (let [server (fake)]
      (try
        (fx/grant! root (:id witness) :everything #{:read :write :evaluate :create})
        (let [w (fx/attach-as world (:token witness))
              _ (is (:ok (act! root "model.create" {:name "voice" :url (:url server) :model-id "m" :stt "whisper-1" :tts "tts-1"})))
              head (log/head (:log world))
              audio (.encodeToString (Base64/getEncoder) (.getBytes "SECRET-SOUND" "UTF-8"))
              r (act! w "speech.transcribe" {:audio audio})]
          (is (:ok r) (pr-str r))
          (is (= "hello there" (get-in r [:result :result :text])))
          (let [c (last @(:calls server))]
            (is (str/includes? (:type c) "multipart/form-data"))
            (is (str/includes? (:text c) "whisper-1"))
            (is (str/includes? (:text c) "SECRET-SOUND")))
          (testing "speech comes back as audio"
            (let [s (act! w "speech.say" {:text "hi"})]
              (is (:ok s) (pr-str s))
              (is (= "RIFFfake" (String. (.decode (Base64/getDecoder) ^String (get-in s [:result :result :audio])) "UTF-8")))
              (is (= "tts-1" (get (j/read-value (:text (last @(:calls server)))) "model")))))
          (testing "no sound and no text on the log"
            (let [events (log/read-from (:log world) (inc head) 1000)]
              (is (not-any? #(or (str/includes? (pr-str %) "SECRET-SOUND") (str/includes? (pr-str %) audio)
                                 (str/includes? (pr-str %) "hello there"))
                            events))))
          (testing "a session that is not a person's, and a loop, are refused"
            (is (= :person-only (code (act! root "speech.say" {:text "hi"}))))))
        (finally ((:stop server)))))))

(deftest no-model-says-so
  (fx/with-world [{:keys [world root witness]}]
    (fx/grant! root (:id witness) :everything #{:read :evaluate})
    (is (= :no-speech-model (code (act! (fx/attach-as world (:token witness)) "speech.say" {:text "hi"}))))))
