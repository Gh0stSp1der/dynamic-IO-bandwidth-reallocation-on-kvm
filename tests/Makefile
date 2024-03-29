export SRC_PATH

check-unit-y = tests/check-qdict$(EXESUF)
gcov-files-check-qdict-y = qobject/qdict.c
check-unit-y += tests/check-qfloat$(EXESUF)
gcov-files-check-qfloat-y = qobject/qfloat.c
check-unit-y += tests/check-qint$(EXESUF)
gcov-files-check-qint-y = qobject/qint.c
check-unit-y += tests/check-qstring$(EXESUF)
gcov-files-check-qstring-y = qobject/qstring.c
check-unit-y += tests/check-qlist$(EXESUF)
gcov-files-check-qlist-y = qobject/qlist.c
check-unit-y += tests/check-qjson$(EXESUF)
gcov-files-check-qjson-y = qobject/qjson.c
check-unit-y += tests/test-qmp-output-visitor$(EXESUF)
gcov-files-test-qmp-output-visitor-y = qapi/qmp-output-visitor.c
check-unit-y += tests/test-qmp-input-visitor$(EXESUF)
gcov-files-test-qmp-input-visitor-y = qapi/qmp-input-visitor.c
check-unit-y += tests/test-qmp-input-strict$(EXESUF)
check-unit-y += tests/test-qmp-commands$(EXESUF)
gcov-files-test-qmp-commands-y = qapi/qmp-dispatch.c
check-unit-y += tests/test-string-input-visitor$(EXESUF)
gcov-files-test-string-input-visitor-y = qapi/string-input-visitor.c
check-unit-y += tests/test-string-output-visitor$(EXESUF)
gcov-files-test-string-output-visitor-y = qapi/string-output-visitor.c
check-unit-y += tests/test-coroutine$(EXESUF)
gcov-files-test-coroutine-y = coroutine-$(CONFIG_COROUTINE_BACKEND).c
check-unit-y += tests/test-visitor-serialization$(EXESUF)
check-unit-y += tests/test-iov$(EXESUF)
gcov-files-test-iov-y = util/iov.c
check-unit-y += tests/test-aio$(EXESUF)
gcov-files-test-aio-$(CONFIG_WIN32) = aio-win32.c
gcov-files-test-aio-$(CONFIG_POSIX) = aio-posix.c
check-unit-y += tests/test-thread-pool$(EXESUF)
gcov-files-test-thread-pool-y = thread-pool.c
gcov-files-test-hbitmap-y = util/hbitmap.c
check-unit-y += tests/test-hbitmap$(EXESUF)
check-unit-y += tests/test-x86-cpuid$(EXESUF)
# all code tested by test-x86-cpuid is inside topology.h
gcov-files-test-x86-cpuid-y =
check-unit-y += tests/test-xbzrle$(EXESUF)
gcov-files-test-xbzrle-y = xbzrle.c
check-unit-y += tests/test-cutils$(EXESUF)
gcov-files-test-cutils-y += util/cutils.c
check-unit-y += tests/test-mul64$(EXESUF)
gcov-files-test-mul64-y = util/host-utils.c

check-block-$(CONFIG_POSIX) += tests/qemu-iotests-quick.sh

# All QTests for now are POSIX-only, but the dependencies are
# really in libqtest, not in the testcases themselves.
check-qtest-i386-y = tests/fdc-test$(EXESUF)
gcov-files-i386-y = hw/fdc.c
check-qtest-i386-y += tests/ide-test$(EXESUF)
check-qtest-i386-y += tests/hd-geo-test$(EXESUF)
gcov-files-i386-y += hw/hd-geometry.c
check-qtest-i386-y += tests/boot-order-test$(EXESUF)
check-qtest-i386-y += tests/rtc-test$(EXESUF)
check-qtest-i386-y += tests/i440fx-test$(EXESUF)
check-qtest-i386-y += tests/fw_cfg-test$(EXESUF)
check-qtest-i386-y += tests/blockdev-test$(EXESUF)
check-qtest-i386-y += tests/qdev-monitor-test$(EXESUF)
check-qtest-x86_64-y = $(check-qtest-i386-y)
gcov-files-i386-y += i386-softmmu/hw/mc146818rtc.c
gcov-files-x86_64-y = $(subst i386-softmmu/,x86_64-softmmu/,$(gcov-files-i386-y))
#check-qtest-sparc-y = tests/m48t59-test$(EXESUF)
#check-qtest-sparc64-y = tests/m48t59-test$(EXESUF)
gcov-files-sparc-y += hw/m48t59.c
gcov-files-sparc64-y += hw/m48t59.c
check-qtest-arm-y = tests/tmp105-test$(EXESUF)
gcov-files-arm-y += hw/tmp105.c

check-qapi-schema-y := $(addprefix tests/qapi-schema/, \
        comments.json empty.json funny-char.json indented-expr.json \
        missing-colon.json missing-comma-list.json \
        missing-comma-object.json non-objects.json \
        qapi-schema-test.json quoted-structural-chars.json \
        trailing-comma-list.json trailing-comma-object.json \
        unclosed-list.json unclosed-object.json unclosed-string.json)

GENERATED_HEADERS += tests/test-qapi-types.h tests/test-qapi-visit.h tests/test-qmp-commands.h

test-obj-y = tests/check-qint.o tests/check-qstring.o tests/check-qdict.o \
	tests/check-qlist.o tests/check-qfloat.o tests/check-qjson.o \
	tests/test-coroutine.o tests/test-string-output-visitor.o \
	tests/test-string-input-visitor.o tests/test-qmp-output-visitor.o \
	tests/test-qmp-input-visitor.o tests/test-qmp-input-strict.o \
	tests/test-qmp-commands.o tests/test-visitor-serialization.o \
	tests/test-x86-cpuid.o tests/test-mul64.o

test-qapi-obj-y = tests/test-qapi-visit.o tests/test-qapi-types.o

$(test-obj-y): QEMU_INCLUDES += -Itests
QEMU_CFLAGS += -I$(SRC_PATH)/tests

tests/test-x86-cpuid.o: QEMU_INCLUDES += -I$(SRC_PATH)/target-i386

tests/check-qint$(EXESUF): tests/check-qint.o libqemuutil.a
tests/check-qstring$(EXESUF): tests/check-qstring.o libqemuutil.a
tests/check-qdict$(EXESUF): tests/check-qdict.o libqemuutil.a
tests/check-qlist$(EXESUF): tests/check-qlist.o libqemuutil.a
tests/check-qfloat$(EXESUF): tests/check-qfloat.o libqemuutil.a
tests/check-qjson$(EXESUF): tests/check-qjson.o libqemuutil.a libqemustub.a
tests/test-coroutine$(EXESUF): tests/test-coroutine.o $(block-obj-y) libqemuutil.a libqemustub.a
tests/test-aio$(EXESUF): tests/test-aio.o $(block-obj-y) libqemuutil.a libqemustub.a
tests/test-thread-pool$(EXESUF): tests/test-thread-pool.o $(block-obj-y) libqemuutil.a libqemustub.a
tests/test-iov$(EXESUF): tests/test-iov.o libqemuutil.a
tests/test-hbitmap$(EXESUF): tests/test-hbitmap.o libqemuutil.a libqemustub.a
tests/test-x86-cpuid$(EXESUF): tests/test-x86-cpuid.o
tests/test-xbzrle$(EXESUF): tests/test-xbzrle.o xbzrle.o page_cache.o libqemuutil.a
tests/test-cutils$(EXESUF): tests/test-cutils.o util/cutils.o

tests/test-qapi-types.c tests/test-qapi-types.h :\
$(SRC_PATH)/tests/qapi-schema/qapi-schema-test.json $(SRC_PATH)/scripts/qapi-types.py
	$(call quiet-command,$(PYTHON) $(SRC_PATH)/scripts/qapi-types.py $(gen-out-type) -o tests -p "test-" < $<, "  GEN   $@")
tests/test-qapi-visit.c tests/test-qapi-visit.h :\
$(SRC_PATH)/tests/qapi-schema/qapi-schema-test.json $(SRC_PATH)/scripts/qapi-visit.py
	$(call quiet-command,$(PYTHON) $(SRC_PATH)/scripts/qapi-visit.py $(gen-out-type) -o tests -p "test-" < $<, "  GEN   $@")
tests/test-qmp-commands.h tests/test-qmp-marshal.c :\
$(SRC_PATH)/tests/qapi-schema/qapi-schema-test.json $(SRC_PATH)/scripts/qapi-commands.py
	$(call quiet-command,$(PYTHON) $(SRC_PATH)/scripts/qapi-commands.py $(gen-out-type) -o tests -p "test-" < $<, "  GEN   $@")

tests/test-string-output-visitor$(EXESUF): tests/test-string-output-visitor.o $(test-qapi-obj-y) libqemuutil.a libqemustub.a
tests/test-string-input-visitor$(EXESUF): tests/test-string-input-visitor.o $(test-qapi-obj-y) libqemuutil.a libqemustub.a
tests/test-qmp-output-visitor$(EXESUF): tests/test-qmp-output-visitor.o $(test-qapi-obj-y) libqemuutil.a libqemustub.a
tests/test-qmp-input-visitor$(EXESUF): tests/test-qmp-input-visitor.o $(test-qapi-obj-y) libqemuutil.a libqemustub.a
tests/test-qmp-input-strict$(EXESUF): tests/test-qmp-input-strict.o $(test-qapi-obj-y) libqemuutil.a libqemustub.a
tests/test-qmp-commands$(EXESUF): tests/test-qmp-commands.o tests/test-qmp-marshal.o $(test-qapi-obj-y) qapi-types.o qapi-visit.o libqemuutil.a libqemustub.a
tests/test-visitor-serialization$(EXESUF): tests/test-visitor-serialization.o $(test-qapi-obj-y) libqemuutil.a libqemustub.a

tests/test-mul64$(EXESUF): tests/test-mul64.o libqemuutil.a

libqos-obj-y = tests/libqos/pci.o tests/libqos/fw_cfg.o
libqos-obj-y += tests/libqos/i2c.o
libqos-pc-obj-y = $(libqos-obj-y) tests/libqos/pci-pc.o tests/libqos/fw_cfg-pc.o
libqos-pc-obj-y += tests/libqos/malloc-pc.o
libqos-omap-obj-y = $(libqos-obj-y) tests/libqos/i2c-omap.o

tests/rtc-test$(EXESUF): tests/rtc-test.o
tests/m48t59-test$(EXESUF): tests/m48t59-test.o
tests/fdc-test$(EXESUF): tests/fdc-test.o
tests/ide-test$(EXESUF): tests/ide-test.o $(libqos-pc-obj-y)
tests/hd-geo-test$(EXESUF): tests/hd-geo-test.o
tests/boot-order-test$(EXESUF): tests/boot-order-test.o
tests/tmp105-test$(EXESUF): tests/tmp105-test.o $(libqos-omap-obj-y)
tests/i440fx-test$(EXESUF): tests/i440fx-test.o $(libqos-pc-obj-y)
tests/fw_cfg-test$(EXESUF): tests/fw_cfg-test.o $(libqos-pc-obj-y)
tests/blockdev-test$(EXESUF): tests/blockdev-test.o $(libqos-pc-obj-y)
tests/qdev-monitor-test$(EXESUF): tests/qdev-monitor-test.o $(libqos-pc-obj-y)

# QTest rules

TARGETS=$(patsubst %-softmmu,%, $(filter %-softmmu,$(TARGET_DIRS)))
QTEST_TARGETS=$(foreach TARGET,$(TARGETS), $(if $(check-qtest-$(TARGET)-y), $(TARGET),))
check-qtest-$(CONFIG_POSIX)=$(foreach TARGET,$(TARGETS), $(check-qtest-$(TARGET)-y))

qtest-obj-y = tests/libqtest.o libqemuutil.a libqemustub.a
$(check-qtest-y): $(qtest-obj-y)

.PHONY: check-help
check-help:
	@echo "Regression testing targets:"
	@echo
	@echo " make check                Run all tests"
	@echo " make check-qtest-TARGET   Run qtest tests for given target"
	@echo " make check-qtest          Run qtest tests"
	@echo " make check-unit           Run qobject tests"
	@echo " make check-qapi-schema    Run QAPI schema tests"
	@echo " make check-block          Run block tests"
	@echo " make check-report.html    Generates an HTML test report"
	@echo
	@echo "Please note that HTML reports do not regenerate if the unit tests"
	@echo "has not changed."
	@echo
	@echo "The variable SPEED can be set to control the gtester speed setting."
	@echo "Default options are -k and (for make V=1) --verbose; they can be"
	@echo "changed with variable GTESTER_OPTIONS."

SPEED = quick
GTESTER_OPTIONS = -k $(if $(V),--verbose,-q)
GCOV_OPTIONS = -n $(if $(V),-f,)

# gtester tests, possibly with verbose output

.PHONY: $(patsubst %, check-qtest-%, $(QTEST_TARGETS))
$(patsubst %, check-qtest-%, $(QTEST_TARGETS)): check-qtest-%: $(check-qtest-y)
	$(if $(CONFIG_GCOV),@rm -f *.gcda */*.gcda */*/*.gcda */*/*/*.gcda,)
	$(call quiet-command,QTEST_QEMU_BINARY=$*-softmmu/qemu-system-$* \
		MALLOC_PERTURB_=$${MALLOC_PERTURB_:-$$((RANDOM % 255 + 1))} \
		gtester $(GTESTER_OPTIONS) -m=$(SPEED) $(check-qtest-$*-y),"GTESTER $@")
	$(if $(CONFIG_GCOV),@for f in $(gcov-files-$*-y); do \
	  echo Gcov report for $$f:;\
	  $(GCOV) $(GCOV_OPTIONS) $$f -o `dirname $$f`; \
	done,)

.PHONY: $(patsubst %, check-%, $(check-unit-y))
$(patsubst %, check-%, $(check-unit-y)): check-%: %
	$(if $(CONFIG_GCOV),@rm -f *.gcda */*.gcda */*/*.gcda */*/*/*.gcda,)
	$(call quiet-command, \
		MALLOC_PERTURB_=$${MALLOC_PERTURB_:-$$((RANDOM % 255 + 1))} \
		gtester $(GTESTER_OPTIONS) -m=$(SPEED) $*,"GTESTER $*")
	$(if $(CONFIG_GCOV),@for f in $(gcov-files-$(subst tests/,,$*)-y); do \
	  echo Gcov report for $$f:;\
	  $(GCOV) $(GCOV_OPTIONS) $$f -o `dirname $$f`; \
	done,)

# gtester tests with XML output

$(patsubst %, check-report-qtest-%.xml, $(QTEST_TARGETS)): check-report-qtest-%.xml: $(check-qtest-y)
	$(call quiet-command,QTEST_QEMU_BINARY=$*-softmmu/qemu-system-$* \
	  gtester -q $(GTESTER_OPTIONS) -o $@ -m=$(SPEED) $(check-qtest-$*-y),"GTESTER $@")

check-report-unit.xml: $(check-unit-y)
	$(call quiet-command,gtester -q $(GTESTER_OPTIONS) -o $@ -m=$(SPEED) $^, "GTESTER $@")

# Reports and overall runs

check-report.xml: $(patsubst %,check-report-qtest-%.xml, $(QTEST_TARGETS)) check-report-unit.xml
	$(call quiet-command,$(SRC_PATH)/scripts/gtester-cat $^ > $@, "  GEN    $@")

check-report.html: check-report.xml
	$(call quiet-command,gtester-report $< > $@, "  GEN    $@")


# Other tests

.PHONY: check-tests/qemu-iotests-quick.sh
check-tests/qemu-iotests-quick.sh: tests/qemu-iotests-quick.sh qemu-img$(EXESUF) qemu-io$(EXESUF)
	$<

.PHONY: check-tests/test-qapi.py
check-tests/test-qapi.py: tests/test-qapi.py

.PHONY: $(patsubst %, check-%, $(check-qapi-schema-y))
$(patsubst %, check-%, $(check-qapi-schema-y)): check-%.json: $(SRC_PATH)/%.json
	$(call quiet-command, PYTHONPATH=$(SRC_PATH)/scripts $(PYTHON) $(SRC_PATH)/tests/qapi-schema/test-qapi.py <$^ >$*.test.out 2>$*.test.err; echo $$? >$*.test.exit, "  TEST  $*.out")
	@diff -q $(SRC_PATH)/$*.out $*.test.out
	@diff -q $(SRC_PATH)/$*.err $*.test.err
	@diff -q $(SRC_PATH)/$*.exit $*.test.exit

# Consolidated targets

.PHONY: check-qapi-schema check-qtest check-unit check
check-qapi-schema: $(patsubst %,check-%, $(check-qapi-schema-y))
check-qtest: $(patsubst %,check-qtest-%, $(QTEST_TARGETS))
check-unit: $(patsubst %,check-%, $(check-unit-y))
check-block: $(patsubst %,check-%, $(check-block-y))
check: check-qapi-schema check-unit check-qtest

-include $(wildcard tests/*.d)
