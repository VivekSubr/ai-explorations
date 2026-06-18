THIRD_PARTY    := $(abspath $(CURDIR)/../third_party)
OPENSSL_SRC    := $(THIRD_PARTY)/openssl
OPENSSL_PREFIX := $(OPENSSL_SRC)/install
OPENSSL_TAG    := openssl-3.5.0

NGHTTP3_SRC    := $(THIRD_PARTY)/nghttp3
NGHTTP3_PREFIX := $(NGHTTP3_SRC)/install

NGTCP2_SRC     := $(THIRD_PARTY)/ngtcp2
NGTCP2_PREFIX  := $(NGTCP2_SRC)/install

CURL_SRC       := $(THIRD_PARTY)/curl/src
CURL_PREFIX    := $(THIRD_PARTY)/curl/install

NPROC          := $(shell nproc 2>/dev/null || echo 2)

# Make ngtcp2's pkg-config see the locally installed openssl/nghttp3.
PKG_CONFIG_PATH_LOCAL := $(OPENSSL_PREFIX)/lib/pkgconfig:$(OPENSSL_PREFIX)/lib64/pkgconfig:$(NGHTTP3_PREFIX)/lib/pkgconfig
CURL_PKG_CONFIG_PATH_LOCAL := $(OPENSSL_PREFIX)/lib/pkgconfig:$(OPENSSL_PREFIX)/lib64/pkgconfig:$(NGHTTP3_PREFIX)/lib/pkgconfig:$(NGTCP2_PREFIX)/lib/pkgconfig
CURL_LD_LIBRARY_PATH_LOCAL := $(OPENSSL_PREFIX)/lib:$(NGHTTP3_PREFIX)/lib:$(NGTCP2_PREFIX)/lib
CURL_LDFLAGS_LOCAL := -Wl,-rpath,$(OPENSSL_PREFIX)/lib -Wl,-rpath,$(NGHTTP3_PREFIX)/lib -Wl,-rpath,$(NGTCP2_PREFIX)/lib

build: openssl nghttp3 ngtcp2

openssl: $(OPENSSL_PREFIX)/.installed

$(OPENSSL_SRC)/Configure:
	@if [ -f "$(OPENSSL_PREFIX)/include/openssl/ssl.h" ]; then \
	    echo "openssl already installed at $(OPENSSL_PREFIX), skipping clone"; \
	else \
	    mkdir -p $(THIRD_PARTY) && \
	    git clone --depth 1 --branch $(OPENSSL_TAG) https://github.com/openssl/openssl.git $(OPENSSL_SRC); \
	fi

$(OPENSSL_PREFIX)/.installed: $(OPENSSL_SRC)/Configure
	@if [ -f "$(OPENSSL_PREFIX)/include/openssl/ssl.h" ] && [ -f "$(OPENSSL_PREFIX)/lib/libssl.so.3" ]; then \
	    echo "openssl already built at $(OPENSSL_PREFIX), skipping"; \
	else \
	    cd $(OPENSSL_SRC) && ./Configure --prefix=$(OPENSSL_PREFIX) --libdir=lib shared enable-ssl-trace && \
	    $(MAKE) -C $(OPENSSL_SRC) -j$(NPROC) && \
	    $(MAKE) -C $(OPENSSL_SRC) install_sw; \
	fi
	mkdir -p $(OPENSSL_PREFIX) && touch $@

nghttp3: $(NGHTTP3_PREFIX)/.installed

$(NGHTTP3_SRC)/CMakeLists.txt:
	@if [ -f "$(NGHTTP3_PREFIX)/include/nghttp3/nghttp3.h" ]; then \
	    echo "nghttp3 already installed at $(NGHTTP3_PREFIX), skipping clone"; \
	else \
	    mkdir -p $(THIRD_PARTY) && \
	    git clone --depth 1 --recursive https://github.com/ngtcp2/nghttp3.git $(NGHTTP3_SRC); \
	fi

$(NGHTTP3_PREFIX)/.installed: $(NGHTTP3_SRC)/CMakeLists.txt
	@if [ -f "$(NGHTTP3_PREFIX)/include/nghttp3/nghttp3.h" ] && [ -f "$(NGHTTP3_PREFIX)/lib/pkgconfig/libnghttp3.pc" ]; then \
	    echo "nghttp3 already built at $(NGHTTP3_PREFIX), skipping"; \
	else \
	    cmake -S $(NGHTTP3_SRC) -B $(NGHTTP3_SRC)/build \
	        -DCMAKE_INSTALL_PREFIX=$(NGHTTP3_PREFIX) \
	        -DCMAKE_INSTALL_LIBDIR=lib \
	        -DCMAKE_BUILD_TYPE=Release \
	        -DENABLE_LIB_ONLY=ON \
	        -DBUILD_SHARED_LIBS=ON && \
	    cmake --build $(NGHTTP3_SRC)/build -j$(NPROC) && \
	    cmake --install $(NGHTTP3_SRC)/build; \
	fi
	mkdir -p $(NGHTTP3_PREFIX) && touch $@

ngtcp2: $(NGTCP2_PREFIX)/.installed

$(NGTCP2_SRC)/CMakeLists.txt:
	@if [ -f "$(NGTCP2_PREFIX)/include/ngtcp2/ngtcp2.h" ]; then \
	    echo "ngtcp2 already installed at $(NGTCP2_PREFIX), skipping clone"; \
	else \
	    mkdir -p $(THIRD_PARTY) && \
	    git clone --depth 1 --recursive https://github.com/ngtcp2/ngtcp2.git $(NGTCP2_SRC); \
	fi

$(NGTCP2_PREFIX)/.installed: $(NGTCP2_SRC)/CMakeLists.txt $(OPENSSL_PREFIX)/.installed $(NGHTTP3_PREFIX)/.installed
	@if [ -f "$(NGTCP2_PREFIX)/include/ngtcp2/ngtcp2.h" ] && [ -f "$(NGTCP2_PREFIX)/lib/pkgconfig/libngtcp2.pc" ] && [ -f "$(NGTCP2_PREFIX)/lib/pkgconfig/libngtcp2_crypto_ossl.pc" ]; then \
	    echo "ngtcp2 already built at $(NGTCP2_PREFIX), skipping"; \
	else \
	    PKG_CONFIG_PATH=$(PKG_CONFIG_PATH_LOCAL):$$PKG_CONFIG_PATH \
	    cmake -S $(NGTCP2_SRC) -B $(NGTCP2_SRC)/build \
	        -DCMAKE_INSTALL_PREFIX=$(NGTCP2_PREFIX) \
	        -DCMAKE_INSTALL_LIBDIR=lib \
	        -DCMAKE_BUILD_TYPE=Release \
	        -DCMAKE_PREFIX_PATH="$(OPENSSL_PREFIX);$(NGHTTP3_PREFIX)" \
	        -DENABLE_OPENSSL=ON \
	        -DENABLE_LIB_ONLY=ON \
	        -DBUILD_SHARED_LIBS=ON && \
	    cmake --build $(NGTCP2_SRC)/build -j$(NPROC) && \
	    cmake --install $(NGTCP2_SRC)/build; \
	fi
	mkdir -p $(NGTCP2_PREFIX) && touch $@

# Build this when the installed curl does not have HTTP/3 support. This target
# is intentionally not part of `make build`; dist_cache_test.sh will prefer the
# resulting third_party/curl/install/bin/curl when it exists.
curl: $(CURL_PREFIX)/.installed

$(CURL_SRC)/configure:
	@if [ -d "$(CURL_SRC)/.git" ]; then \
	    echo "curl source already present at $(CURL_SRC), skipping clone"; \
	else \
	    mkdir -p $(THIRD_PARTY)/curl && \
	    git clone --depth 1 https://github.com/curl/curl.git $(CURL_SRC); \
	fi
	cd $(CURL_SRC) && autoreconf -fi

$(CURL_PREFIX)/.installed: $(CURL_SRC)/configure $(OPENSSL_PREFIX)/.installed $(NGHTTP3_PREFIX)/.installed $(NGTCP2_PREFIX)/.installed
	@if [ -x "$(CURL_PREFIX)/bin/curl" ] && "$(CURL_PREFIX)/bin/curl" -V | grep -q HTTP3; then \
	    echo "curl with HTTP/3 already built at $(CURL_PREFIX), skipping"; \
	else \
	    cd $(CURL_SRC) && \
	    PKG_CONFIG_PATH=$(CURL_PKG_CONFIG_PATH_LOCAL):$$PKG_CONFIG_PATH \
	    LD_LIBRARY_PATH=$(CURL_LD_LIBRARY_PATH_LOCAL):$$LD_LIBRARY_PATH \
	    LDFLAGS="$(CURL_LDFLAGS_LOCAL) $$LDFLAGS" \
	    ./configure \
	        --prefix=$(CURL_PREFIX) \
	        --with-openssl=$(OPENSSL_PREFIX) \
	        --with-nghttp3=$(NGHTTP3_PREFIX) \
	        --with-ngtcp2=$(NGTCP2_PREFIX) \
	        --without-libpsl \
	        --without-libidn2 \
	        --without-brotli \
	        --without-zstd \
	        --without-zlib \
	        --without-libssh2 \
	        --without-librtmp \
	        --disable-ldap \
	        --disable-ldaps \
	        --disable-rtsp \
	        --disable-dict \
	        --disable-telnet \
	        --disable-tftp \
	        --disable-pop3 \
	        --disable-imap \
	        --disable-smtp \
	        --disable-gopher \
	        --disable-mqtt \
	        --disable-manual \
	        --disable-dependency-tracking && \
	    $(MAKE) -C $(CURL_SRC) -j$(NPROC) && \
	    $(MAKE) -C $(CURL_SRC) install; \
	fi
	mkdir -p $(CURL_PREFIX) && touch $@

clean:
	$(MAKE) -C server clean

# Wipes third_party/ as well, forcing openssl/nghttp3/ngtcp2 to be re-cloned
# and rebuilt on the next `make build`. Use sparingly.
distclean: clean
	rm -fr third_party

# End-to-end test driver: brings up redis + dist_cache.exe and exercises the
# REST API. Forwarded args let you filter, e.g.
#   make test ARGS='TC_KV_*'
test:
	./dist_cache_test.sh $(ARGS)

# .PHONY is a legacy feature of make - it declares 'clean' is target, there will be no file named 'clean'
.PHONY: clean distclean test build openssl nghttp3 ngtcp2 curl