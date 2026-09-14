package("libnice")
    set_kind("library")
    set_homepage("https://libnice.freedesktop.org/")
    set_description("libnice is an implementation of the IETF's Interactive Connectivity Establishment (ICE) standard")
    set_license("LGPL-2.1-or-later")

    add_urls("https://gitlab.freedesktop.org/libnice/libnice/-/archive/$(version)/libnice-$(version).tar.gz")
    add_versions("0.1.24", "1da5ac13ed5d4e175e0d2d46ad3748c6635244f8f3eb2b8e31578ef59aa2ddce")
    add_patches("0.1.24", path.join(os.scriptdir(), "patches", "relay_upgrade_0.1.24.patch"),
        "81c7acc267a044fd6d0053f0c4d0bbdc414d1032b0e51e072a7e93e8bf85bc7a")
    add_patches("0.1.24", path.join(os.scriptdir(), "patches", "turn_close_receive_0.1.24.patch"),
        "87e061a6d41c322827bbe7138707df546cd70b9c228d689d0edf5462370df578")
    add_patches("0.1.24", path.join(os.scriptdir(), "patches", "multi_stun_0.1.24.patch"),
        "a098c4e52379bb8f3067f4c65c1a48748b6dfb515f667a1d1d931744062e381b")
    add_patches("0.1.24", path.join(os.scriptdir(), "patches", "udp_punch_0.1.24.patch"),
        "569870d7fb948c1696bb93cf89cbbd8a189eac6df7631c4b87333e344f7c29e0")
    add_configs("udp_punch_revision", {description = "MiniRTC bounded UDP punch backend", default = "1", type = "string", readonly = true})
    add_configs("turn_close_receive", {description = "Receive TURN replies while closing streams", default = true, type = "boolean", readonly = true})
    -- Include the extension in the package identity to invalidate old binaries.
    add_configs("relay_upgrade", {description = "MiniRTC negotiated relay upgrade extension", default = true, type = "boolean", readonly = true})
    add_configs("multi_stun", {description = "Same-socket multi-endpoint STUN discovery", default = true, type = "boolean", readonly = true})

    add_deps("meson~host", "pkgconf", {host = true})
    add_deps("glib 2.84.1", "openssl3 3.3.2")
    add_deps("gupnp-igd 1.6.0", {system = false, configs = {shared = false}})

    add_configs("include_virtual_interfaces", {description = "Gather candidates from VPN/TUN interfaces", default = false, type = "boolean"})

    on_install(function (package)
        -- libnice 0.1.24 still requests the older pkg-config API name.
        -- GUPnP IGD 1.6 retains the C API used by libnice and uses libsoup 3.
        io.replace("meson.build", "dependency('gupnp-igd-1.0',",
            "dependency('gupnp-igd-1.6',", {plain = true})
        if package:is_plat("windows") then
            io.replace("meson.build",
                "syslibs += [cc.find_library('ws2_32')]",
                "syslibs += [cc.find_library('ws2_32')]\n  syslibs += [cc.find_library('crypt32')]",
                {plain = true})
        end

        local  configs = {
            "-Ddefault_library=static",
            "-Dgstreamer=disabled",
            "-Dexamples=disabled",
            "-Dtests=disabled",
            "-Dgtk_doc=disabled",
            "-Dcrypto-library=openssl",
            "-Dintrospection=disabled",
            "-Dgupnp=enabled"
        }

        if not package:config("include_virtual_interfaces") then
            table.insert(configs,
                "-Dignored-network-interface-prefix=docker,veth,virbr,vnet,utun,tun,tap,wg,Wintun,WireGuard,Tailscale,tailscale,ZeroTier,zerotier")
        end

        if package:is_plat("macosx", "iphoneos") then
            io.replace("socket/udp-bsd.c",
                "#endif\n\n  if (recv_tos) {",
                [[#endif

#if defined(__APPLE__) && defined(IP_BOUND_IF) && defined(IPV6_BOUND_IF)
  /* A source-address bind alone can still follow a utun default route on
   * Apple platforms. Pin each ICE socket to the interface that owns its
   * candidate address, matching the Windows IP_UNICAST_IF behavior above. */
  if (addr) {
    guint if_index = nice_interfaces_get_if_index_by_addr (addr);
    if (if_index) {
      guint level = nice_address_ip_version (addr) == 6 ? IPPROTO_IPV6 : IPPROTO_IP;
      guint optname = nice_address_ip_version (addr) == 6 ? IPV6_BOUND_IF : IP_BOUND_IF;
      GError *gerr = NULL;
      if (!g_socket_set_option (gsock, level, optname, if_index, &gerr)) {
        nice_debug ("Could not bind Apple socket to interface: %s", gerr->message);
        g_clear_error (&gerr);
      }
    }
  }
#endif

  if (recv_tos) {]],
                {plain = true})
        end

        import("package.tools.meson").install(package, configs)
    end)

    on_test(function (package)
        assert(package:has_cfuncs("nice_agent_new", {includes = "nice/agent.h"}))
    end)
