(ns knowledge.world.act.copy-field-test
  "set-field from: a model keeps a whole document without retyping it."
  (:require [clojure.string :as str]
            [clojure.test :refer [deftest is testing]]
            [knowledge.world.fixtures :as fx]
            [knowledge.world.ws-client :as ws]))

(defn- fields [c id] (get-in (ws/request!! c :read {:object id}) [:result :object/fields]))
(defn- resolve-as [c n] (get-in (ws/request!! c :resolve {:name n :op :read}) [:result :object]))

(deftest sixteen-kb-from-a-task-is-byte-identical
  (fx/with-world [{:keys [root]}]
    (let [root-id (:principal root)
          shell (resolve-as root "shell.local")
          doc (apply str (for [i (range 400)] (str "line " i " of the soul, with quite a lot of words on it\n")))
          ;; inside the Adapter's root: a confined command reads nothing else
          locator (get-in (fields root shell) [:adapter/locator :field/value])
          file (doto (java.io.File/createTempFile "soul" ".md" (java.io.File. ^String locator)) (.deleteOnExit))
          path (str file)
          _ (spit path doc)
          run (ws/request!! root :act {:object shell :action :run :args {:cmd (str "cat " path)}})
          task (or (get-in run [:result :task]) (get-in run [:result :result :task]) (get-in run [:object]))]
      (is (:ok run) (pr-str run))
      (is (> (count doc) 16000))
      (let [r (ws/request!! root :set-field {:object root-id :field :instructions/soul :from {:object task}})]
        (is (:ok r) (pr-str r))
        (is (= doc (get-in (fields root root-id) [:instructions/soul :field/value])))
        (is (= :string (get-in (fields root root-id) [:instructions/soul :field/type]))))
      (testing "a range copies a page"
        (let [r (ws/request!! root :set-field {:object root-id :field :notes/page :from {:object task :range [1 2]}})]
          (is (:ok r) (pr-str r))
          (is (= "line 0 of the soul, with quite a lot of words on it\nline 1 of the soul, with quite a lot of words on it"
                 (str/trimr (get-in (fields root root-id) [:notes/page :field/value]))))))
      (testing "what you cannot read you cannot copy"
        (is (= :unresolved (get-in (ws/request!! root :set-field {:object root-id :field :notes/x :from {:object (random-uuid)}}) [:err :code])))))))
