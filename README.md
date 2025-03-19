`da-tcmalloc`
===========

Fork of original crate [tcmalloc-rs](https://crates.io/crates/tcmalloc) with several differences.

- `gperftools` rebased to a much newer version 2.16 (from 2024), compared to 2.7 (2018)
- Fixed broken static compilation from `tcmalloc-rs` fork point (`tcmalloc-rs` produce binaries that dynamically linked to `tcmalloc.so`)
- Assume that libc `malloc` is replaced for the entire process, no need to provide Rust-level `GlobalAlloc`
- Always bundle a modified `tsmalloc` that is disengaged from environment variables (and so cannot be affected by them). Instead, allow to set configuration variables via API
- Allow setting exact path for `tcmalloc`'s' memprofile dumps
- Expose `tcmalloc` API calls such as `release_free_memory` (give back unused memory to the OS)


### Example usage

```rust
fn main() {
    da_tcmalloc::start("/tmp/unused".into());

    let mut x = vec![];
    for _ in 0..1000 {
        let mut v: Vec<u8> = vec![];
        v.resize(0x2, 0);
        x.push(v);
    }

    da_tcmalloc::set_exact_path("/tmp/dump-path.txt".into());

    da_tcmalloc::dump("manual");

    da_tcmalloc::stop();
}
```

## Issues

I see that programs the following in `build.rs` despite having
`cargo:rustc-link-lib=stdc++` in the `build.rs` of `da-tcmalloc-sys`. If you
find why is that, please send a PR.

```rust
fn main() {
    println!("cargo:rustc-link-lib=stdc++");
    println!("cargo:rustc-link-lib=unwind");
}
```


## License

Interact is licensed under either of

 * Apache License, Version 2.0, ([LICENSE-APACHE](LICENSE-APACHE) or
   http://www.apache.org/licenses/LICENSE-2.0)
 * MIT license ([LICENSE-MIT](LICENSE-MIT) or
   http://opensource.org/licenses/MIT)

at your option.



### Contribution

Unless you explicitly state otherwise, any contribution intentionally submitted
for inclusion in Interact by you, as defined in the Apache-2.0 license, shall be
dual licensed as above, without any additional terms or conditions.
