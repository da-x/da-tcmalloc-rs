`da-tcmalloc`
===========

Fork of original crate [tcmalloc-rs](https://crates.io/crates/tcmalloc) with several differences.

- Assume libc `malloc` is replaced for the entire process, no need to provide `GlobalAlloc`
- Always a bundled modified `tsmalloc` that is disengage from environment
  allows setting of configuration variables via API
- Allow setting exact path for memprofile dump
- Fixed broken static compilation from `tcmalloc-rs` fork point


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
