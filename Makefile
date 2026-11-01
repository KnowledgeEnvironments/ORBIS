# ORBIS: world (Clojure) + listener (C++). `make test` is the CI entry.
export JAVA_HOME ?= /opt/homebrew/opt/openjdk@25
export PATH := $(JAVA_HOME)/bin:/opt/homebrew/bin:$(PATH)
CLJ := clojure

.PHONY: smoke holder test-holder deps world nrepl listener android test test-schema test-unit test-exit kz schema-json schema-check fmt

holder:          ## the terminal holder: needs Zig, see holder/PIN
	$(MAKE) -C holder

test-holder: holder  ## the holder on its own, over its socket
	python3 holder/tests/holder_test.py holder/build/holder

deps:            ## resolve all Clojure deps
	cd schema && $(CLJ) -P -M:test
	cd world  && $(CLJ) -P -M:dev:test:nrepl

world:           ## run the world on KE_PORT (default 7777) with KE_DATA (default ./data)
	cd world && $(CLJ) -M:run

nrepl:           ## nREPL on 7888 with dev + test paths
	cd world && $(CLJ) -M:dev:nrepl

listener:        ## configure + build the listener (debug)
	cd listener && cmake --preset macos-debug && cmake --build --preset macos-debug

# Gradle 8.12 refuses JDK 25, hence its own JDK. The SDK needs platform 35; Gradle installs the
# pinned NDK and CMake itself once the licenses are accepted (listener/README.md).
ANDROID_JDK ?= /opt/homebrew/opt/openjdk@21
ANDROID_HOME ?= $(HOME)/Library/Android/sdk

android:         ## debug APK: listener/platform/android/app/build/outputs/apk/debug/app-debug.apk
	cd listener/platform/android && JAVA_HOME=$(ANDROID_JDK) ANDROID_HOME=$(ANDROID_HOME) ./gradlew --no-daemon assembleDebug

test: test-schema test-unit test-exit kz

test-schema:
	cd schema && $(CLJ) -M:test

smoke:           ## before a deploy, about two minutes: a wake, a prompt, a gate, a boot. The full suite is for a release
	cd world && $(CLJ) -M:test --focus knowledge.world.loop.answer-test --focus knowledge.world.loop.worklog-test --focus knowledge.world.loop.realwork-test --focus knowledge.world.serialize.wake-economy-test --focus knowledge.world.act.gates-test --focus knowledge.world.window-test --focus knowledge.world.adapters.git-test --focus knowledge.world.protocol.resume-test --focus knowledge.world.retention-test

test-unit: holder
	cd world && $(CLJ) -M:test --focus :unit

test-exit:       ## sprint 0 exit test: attach, drop, reattach, no gap
	cd world && $(CLJ) -M:test --focus :exit

kz:              ## kernel-zero: report-only until it goes green (leading '-')
	-cd world && $(CLJ) -M:test --focus :kernel-zero

schema-json:     ## export Malli schemas to JSON Schema for the listener; commit what changes
	cd schema && $(CLJ) -X:export

schema-check: schema-json  ## CI: fails if the committed export does not match the schemas
	git diff --exit-code -- listener/generated/schema

fmt:
	find listener/src listener/tests listener/tools -name '*.[ch]pp' | xargs clang-format -i
