# JSONCPP
ifdef HAVE_ANDROID
JSONCPP_VERSION := 1.9.3
else
ifdef HAVE_LINUX
JSONCPP_VERSION := 1.7.2
else
JSONCPP_VERSION := 1.9.3
endif
endif

JSONCPP_URL := https://github.com/open-source-parsers/jsoncpp/archive/$(JSONCPP_VERSION).tar.gz

PKGS += jsoncpp

ifeq ($(call need_pkg,"jsoncpp >= 1.7.2"),)
PKGS_FOUND += jsoncpp
endif

JSONCPP_CMAKECONF := -DBUILD_STATIC_LIBS:BOOL=ON \
                     -DBUILD_SHARED_LIBS:BOOL=OFF \
                     -DJSONCPP_WITH_TESTS:BOOL=OFF

$(TARBALLS)/jsoncpp-$(JSONCPP_VERSION).tar.gz:
	$(call download,$(JSONCPP_URL))

.sum-jsoncpp: jsoncpp-$(JSONCPP_VERSION).tar.gz

jsoncpp: jsoncpp-$(JSONCPP_VERSION).tar.gz .sum-jsoncpp
	$(UNPACK)
	$(MOVE)

.jsoncpp: jsoncpp toolchain.cmake
	cd $< && $(HOSTVARS) $(CMAKE) ${JSONCPP_CMAKECONF}
	cd $< && $(MAKE) install
	touch $@
