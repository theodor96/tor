// Copyright (c) 2016-2017, The Tor Project, Inc. */
// See LICENSE for licensing information */

use std::collections::HashMap;
use std::collections::hash_map;
use std::fmt;
use std::str;
use std::str::FromStr;
use std::string::String;

use tor_util::strings::NUL_BYTE;
use external::c_tor_version_as_new_as;

use errors::ProtoverError;
use protoset::Version;
use protoset::ProtoSet;

/// The first version of Tor that included "proto" entries in its descriptors.
/// Authorities should use this to decide whether to guess proto lines.
///
/// C_RUST_COUPLED:
///     src/or/protover.h `FIRST_TOR_VERSION_TO_ADVERTISE_PROTOCOLS`
const FIRST_TOR_VERSION_TO_ADVERTISE_PROTOCOLS: &'static str = "0.2.9.3-alpha";

/// The maximum number of subprotocol version numbers we will attempt to expand
/// before concluding that someone is trying to DoS us
///
/// C_RUST_COUPLED: src/or/protover.c `MAX_PROTOCOLS_TO_EXPAND`
pub(crate) const MAX_PROTOCOLS_TO_EXPAND: usize = (1<<16);

/// Currently supported protocols and their versions, as a byte-slice.
///
/// # Warning
///
/// This byte-slice ends in a NUL byte.  This is so that we can directly convert
/// it to an `&'static CStr` in the FFI code, in order to hand the static string
/// to C in a way that is compatible with C static strings.
///
/// Rust code which wishes to accesses this string should use
/// `protover::get_supported_protocols()` instead.
///
/// C_RUST_COUPLED: src/or/protover.c `protover_get_supported_protocols`
pub(crate) const SUPPORTED_PROTOCOLS: &'static [u8] =
    b"Cons=1-2 \
    Desc=1-2 \
    DirCache=1-2 \
    HSDir=1-2 \
    HSIntro=3-4 \
    HSRend=1-2 \
    Link=1-5 \
    LinkAuth=1,3 \
    Microdesc=1-2 \
    Relay=1-2\0";

/// Known subprotocols in Tor. Indicates which subprotocol a relay supports.
///
/// C_RUST_COUPLED: src/or/protover.h `protocol_type_t`
#[derive(Clone, Hash, Eq, PartialEq, Debug)]
pub enum Protocol {
    Cons,
    Desc,
    DirCache,
    HSDir,
    HSIntro,
    HSRend,
    Link,
    LinkAuth,
    Microdesc,
    Relay,
}

impl fmt::Display for Protocol {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "{:?}", self)
    }
}

/// Translates a string representation of a protocol into a Proto type.
/// Error if the string is an unrecognized protocol name.
///
/// C_RUST_COUPLED: src/or/protover.c `PROTOCOL_NAMES`
impl FromStr for Protocol {
    type Err = ProtoverError;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        match s {
            "Cons" => Ok(Protocol::Cons),
            "Desc" => Ok(Protocol::Desc),
            "DirCache" => Ok(Protocol::DirCache),
            "HSDir" => Ok(Protocol::HSDir),
            "HSIntro" => Ok(Protocol::HSIntro),
            "HSRend" => Ok(Protocol::HSRend),
            "Link" => Ok(Protocol::Link),
            "LinkAuth" => Ok(Protocol::LinkAuth),
            "Microdesc" => Ok(Protocol::Microdesc),
            "Relay" => Ok(Protocol::Relay),
            _ => Err(ProtoverError::UnknownProtocol),
        }
    }
}

/// A protocol string which is not one of the `Protocols` we currently know
/// about.
#[derive(Clone, Debug, Hash, Eq, PartialEq)]
pub struct UnknownProtocol(String);

impl fmt::Display for UnknownProtocol {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "{}", self.0)
    }
}

impl FromStr for UnknownProtocol {
    type Err = ProtoverError;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        Ok(UnknownProtocol(s.to_string()))
    }
}

impl From<Protocol> for UnknownProtocol {
    fn from(p: Protocol) -> UnknownProtocol {
        UnknownProtocol(p.to_string())
    }
}

/// Get the string representation of current supported protocols
///
/// # Returns
///
/// A `String` whose value is the existing protocols supported by tor.
/// Returned data is in the format as follows:
///
/// "HSDir=1-1 LinkAuth=1"
///
pub fn get_supported_protocols() -> &'static str {
    // The `len() - 1` is to remove the NUL byte.
    // The `unwrap` is safe becauase we SUPPORTED_PROTOCOLS is under
    // our control.
    str::from_utf8(&SUPPORTED_PROTOCOLS[..SUPPORTED_PROTOCOLS.len() - 1])
        .unwrap_or("")
}

/// A map of protocol names to the versions of them which are supported.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ProtoEntry(HashMap<Protocol, ProtoSet>);

impl Default for ProtoEntry {
    fn default() -> ProtoEntry {
        ProtoEntry( HashMap::new() )
    }
}

impl ProtoEntry {
    /// Get an iterator over the `Protocol`s and their `ProtoSet`s in this `ProtoEntry`.
    pub fn iter(&self) -> hash_map::Iter<Protocol, ProtoSet> {
        self.0.iter()
    }

    /// Translate the supported tor versions from a string into a
    /// ProtoEntry, which is useful when looking up a specific
    /// subprotocol.
    pub fn supported() -> Result<Self, ProtoverError> {
        let supported: &'static str = get_supported_protocols();

        supported.parse()
    }

    pub fn get(&self, protocol: &Protocol) -> Option<&ProtoSet> {
        self.0.get(protocol)
    }

    pub fn insert(&mut self, key: Protocol, value: ProtoSet) {
        self.0.insert(key, value);
    }

    pub fn remove(&mut self, key: &Protocol) -> Option<ProtoSet> {
        self.0.remove(key)
    }

    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }
}

impl FromStr for ProtoEntry {
    type Err = ProtoverError;

    /// Parse a string of subprotocol types and their version numbers.
    ///
    /// # Inputs
    ///
    /// * A `protocol_entry` string, comprised of a keywords, an "=" sign, and
    /// one or more version numbers, each separated by a space.  For example,
    /// `"Cons=3-4 HSDir=1"`.
    ///
    /// # Returns
    ///
    /// A `Result` whose `Ok` value is a `ProtoEntry`, where the
    /// first element is the subprotocol type (see `protover::Protocol`) and the last
    /// element is an ordered set of `(low, high)` unique version numbers which are supported.
    /// Otherwise, the `Err` value of this `Result` is a `ProtoverError`.
    fn from_str(protocol_entry: &str) -> Result<ProtoEntry, ProtoverError> {
        let mut proto_entry: ProtoEntry = ProtoEntry::default();
        let entries = protocol_entry.split(' ');

        for entry in entries {
            let mut parts = entry.splitn(2, '=');

            let proto = match parts.next() {
                Some(n) => n,
                None => return Err(ProtoverError::Unparseable),
            };

            let vers = match parts.next() {
                Some(n) => n,
                None => return Err(ProtoverError::Unparseable),
            };
            let versions: ProtoSet = vers.parse()?;
            let proto_name: Protocol = proto.parse()?;

            proto_entry.insert(proto_name, versions);
        }

        Ok(proto_entry)
    }
}

/// Parses a single subprotocol entry string into subprotocol and version
/// parts, and then checks whether any of those versions are unsupported.
/// Helper for protover::all_supported
///
/// # Inputs
///
/// Accepted data is in the string format as follows:
///
/// "HSDir=1-1"
///
/// # Returns
///
/// Returns `true` if the protocol entry is well-formatted and only contains
/// versions that are also supported in tor. Otherwise, returns false
///
fn contains_only_supported_protocols(proto_entry: &str) -> bool {
    let (name, mut vers) = match get_proto_and_vers(proto_entry) {
        Ok(n) => n,
        Err(_) => return false,
    };

    let currently_supported = match SupportedProtocols::tor_supported() {
        Ok(n) => n.0,
        Err(_) => return false,
    };

    let supported_versions = match currently_supported.get(&name) {
        Some(n) => n,
        None => return false,
    };

    vers.0.retain(|x| !supported_versions.0.contains(x));
    vers.0.is_empty()
}

/// Determine if we support every protocol a client supports, and if not,
/// determine which protocols we do not have support for.
///
/// # Inputs
///
/// Accepted data is in the string format as follows:
///
/// "HSDir=1-1 LinkAuth=1-2"
///
/// # Returns
///
/// Return `true` if every protocol version is one that we support.
/// Otherwise, return `false`.
/// Optionally, return parameters which the client supports but which we do not
///
/// # Examples
/// ```
/// use protover::all_supported;
///
/// let (is_supported, unsupported)  = all_supported("Link=1");
/// assert_eq!(true, is_supported);
///
/// let (is_supported, unsupported)  = all_supported("Link=5-6");
/// assert_eq!(false, is_supported);
/// assert_eq!("Link=5-6", unsupported);
///
pub fn all_supported(protocols: &str) -> (bool, String) {
    let unsupported = protocols
        .split_whitespace()
        .filter(|v| !contains_only_supported_protocols(v))
        .collect::<Vec<&str>>();

    (unsupported.is_empty(), unsupported.join(" "))
}

/// Return true iff the provided protocol list includes support for the
/// indicated protocol and version.
/// Otherwise, return false
///
/// # Inputs
///
/// * `list`, a string representation of a list of protocol entries.
/// * `proto`, a `Proto` to test support for
/// * `vers`, a `Version` version which we will go on to determine whether the
/// specified protocol supports.
///
/// # Examples
/// ```
/// use protover::*;
///
/// let is_supported = protover_string_supports_protocol("Link=3-4 Cons=1",
///                                                      Proto::Cons,1);
/// assert_eq!(true, is_supported);
///
/// let is_not_supported = protover_string_supports_protocol("Link=3-4 Cons=1",
///                                                           Proto::Cons,5);
/// assert_eq!(false, is_not_supported)
/// ```
pub fn protover_string_supports_protocol(
    list: &str,
    proto: Proto,
    vers: Version,
) -> bool {
    let supported = match SupportedProtocols::from_proto_entries_string(list) {
        Ok(result) => result.0,
        Err(_) => return false,
    };

    let supported_versions = match supported.get(&proto) {
        Some(n) => n,
        None => return false,
    };

    supported_versions.0.contains(&vers)
}

/// As protover_string_supports_protocol(), but also returns True if
/// any later version of the protocol is supported.
///
/// # Examples
/// ```
/// use protover::*;
///
/// let is_supported = protover_string_supports_protocol_or_later(
///                       "Link=3-4 Cons=5", Proto::Cons, 5);
///
/// assert_eq!(true, is_supported);
///
/// let is_supported = protover_string_supports_protocol_or_later(
///                       "Link=3-4 Cons=5", Proto::Cons, 4);
///
/// assert_eq!(true, is_supported);
///
/// let is_supported = protover_string_supports_protocol_or_later(
///                       "Link=3-4 Cons=5", Proto::Cons, 6);
///
/// assert_eq!(false, is_supported);
/// ```
pub fn protover_string_supports_protocol_or_later(
    list: &str,
    proto: Proto,
    vers: u32,
) -> bool {
    let supported = match SupportedProtocols::from_proto_entries_string(list) {
        Ok(result) => result.0,
        Err(_) => return false,
    };

    let supported_versions = match supported.get(&proto) {
        Some(n) => n,
        None => return false,
    };

    supported_versions.0.iter().any(|v| v >= &vers)
}

/// Parses a protocol list without validating the protocol names
///
/// # Inputs
///
/// * `protocol_string`, a string comprised of keys and values, both which are
/// strings. The keys are the protocol names while values are a string
/// representation of the supported versions.
///
/// The input is _not_ expected to be a subset of the Proto types
///
/// # Returns
///
/// A `Result` whose `Ok` value is a `HashSet<Version>` holding all of the
/// unique version numbers.
///
/// The returned `Result`'s `Err` value is an `&'static str` with a description
/// of the error.
///
/// # Errors
///
/// This function will error if:
///
/// * The protocol string does not follow the "protocol_name=version_list"
/// expected format
/// * If the version string is malformed. See `Versions::from_version_string`.
///
fn parse_protocols_from_string_with_no_validation<'a>(
    protocol_string: &'a str,
) -> Result<HashMap<String, Versions>, &'static str> {
    let mut parsed: HashMap<String, Versions> = HashMap::new();

    for subproto in protocol_string.split(" ") {
        let mut parts = subproto.splitn(2, "=");

        let name = match parts.next() {
            Some("") => return Err("invalid protover entry"),
            Some(n) => n,
            None => return Err("invalid protover entry"),
        };

        let vers = match parts.next() {
            Some(n) => n,
            None => return Err("invalid protover entry"),
        };

        let versions = Versions::from_version_string(vers)?;

        parsed.insert(String::from(name), versions);
    }
    Ok(parsed)
}

/// Protocol voting implementation.
///
/// Given a list of strings describing protocol versions, return a new
/// string encoding all of the protocols that are listed by at
/// least threshold of the inputs.
///
/// The string is sorted according to the following conventions:
///   - Protocols names are alphabetized
///   - Protocols are in order low to high
///   - Individual and ranges are listed together. For example,
///     "3, 5-10,13"
///   - All entries are unique
///
/// # Examples
/// ```
/// use protover::compute_vote;
///
/// let protos = vec![String::from("Link=3-4"), String::from("Link=3")];
/// let vote = compute_vote(protos, 2);
/// assert_eq!("Link=3", vote)
/// ```
pub fn compute_vote(
    list_of_proto_strings: Vec<String>,
    threshold: i32,
) -> String {
    let empty = String::from("");

    if list_of_proto_strings.is_empty() {
        return empty;
    }

    // all_count is a structure to represent the count of the number of
    // supported versions for a specific protocol. For example, in JSON format:
    // {
    //  "FirstSupportedProtocol": {
    //      "1": "3",
    //      "2": "1"
    //  }
    // }
    // means that FirstSupportedProtocol has three votes which support version
    // 1, and one vote that supports version 2
    let mut all_count: HashMap<String, HashMap<Version, usize>> =
        HashMap::new();

    // parse and collect all of the protos and their versions and collect them
    for vote in list_of_proto_strings {
        let this_vote: HashMap<String, Versions> =
            match parse_protocols_from_string_with_no_validation(&vote) {
                Ok(result) => result,
                Err(_) => continue,
            };
        for (protocol, versions) in this_vote {
            let supported_vers: &mut HashMap<Version, usize> =
                all_count.entry(protocol).or_insert(HashMap::new());

            for version in versions.0 {
                let counter: &mut usize =
                    supported_vers.entry(version).or_insert(0);
                *counter += 1;
            }
        }
    }

    let mut final_output: HashMap<String, String> =
        HashMap::with_capacity(get_supported_protocols().split(" ").count());

    // Go through and remove verstions that are less than the threshold
    for (protocol, versions) in all_count {
        let mut meets_threshold = HashSet::new();
        for (version, count) in versions {
            if count >= threshold as usize {
                meets_threshold.insert(version);
            }
        }

        // For each protocol, compress its version list into the expected
        // protocol version string format
        let contracted = contract_protocol_list(&meets_threshold);
        if !contracted.is_empty() {
            final_output.insert(protocol, contracted);
        }
    }

    write_vote_to_string(&final_output)
}

/// Return a String comprised of protocol entries in alphabetical order
///
/// # Inputs
///
/// * `vote`, a `HashMap` comprised of keys and values, both which are strings.
/// The keys are the protocol names while values are a string representation of
/// the supported versions.
///
/// # Returns
///
/// A `String` whose value is series of pairs, comprising of the protocol name
/// and versions that it supports. The string takes the following format:
///
/// "first_protocol_name=1,2-5, second_protocol_name=4,5"
///
/// Sorts the keys in alphabetical order and creates the expected subprotocol
/// entry format.
///
fn write_vote_to_string(vote: &HashMap<String, String>) -> String {
    let mut keys: Vec<&String> = vote.keys().collect();
    keys.sort();

    let mut output = Vec::new();
    for key in keys {
        // TODO error in indexing here?
        output.push(format!("{}={}", key, vote[key]));
    }
    output.join(" ")
}

/// Returns a boolean indicating whether the given protocol and version is
/// supported in any of the existing Tor protocols
///
/// # Examples
/// ```
/// use protover::*;
///
/// let is_supported = is_supported_here(Proto::Link, 10);
/// assert_eq!(false, is_supported);
///
/// let is_supported = is_supported_here(Proto::Link, 1);
/// assert_eq!(true, is_supported);
/// ```
pub fn is_supported_here(proto: Proto, vers: Version) -> bool {
    let currently_supported = match SupportedProtocols::tor_supported() {
        Ok(result) => result.0,
        Err(_) => return false,
    };

    let supported_versions = match currently_supported.get(&proto) {
        Some(n) => n,
        None => return false,
    };

    supported_versions.0.contains(&vers)
}

/// Older versions of Tor cannot infer their own subprotocols
/// Used to determine which subprotocols are supported by older Tor versions.
///
/// # Inputs
///
/// * `version`, a string comprised of "[0-9a-z.-]"
///
/// # Returns
///
/// A `&'static [u8]` encoding a list of protocol names and supported
/// versions. The string takes the following format:
///
/// "HSDir=1-1 LinkAuth=1"
///
/// This function returns the protocols that are supported by the version input,
/// only for tor versions older than FIRST_TOR_VERSION_TO_ADVERTISE_PROTOCOLS.
///
/// C_RUST_COUPLED: src/rust/protover.c `compute_for_old_tor`
pub fn compute_for_old_tor(version: &str) -> &'static [u8] {
    if c_tor_version_as_new_as(version, FIRST_TOR_VERSION_TO_ADVERTISE_PROTOCOLS) {
        return NUL_BYTE;
    }

    if c_tor_version_as_new_as(version, "0.2.9.1-alpha") {
        return b"Cons=1-2 Desc=1-2 DirCache=1 HSDir=1 HSIntro=3 HSRend=1-2 \
                 Link=1-4 LinkAuth=1 Microdesc=1-2 Relay=1-2\0";
    }

    if c_tor_version_as_new_as(version, "0.2.7.5") {
        return b"Cons=1-2 Desc=1-2 DirCache=1 HSDir=1 HSIntro=3 HSRend=1 \
                 Link=1-4 LinkAuth=1 Microdesc=1-2 Relay=1-2\0";
    }

    if c_tor_version_as_new_as(version, "0.2.4.19") {
        return b"Cons=1 Desc=1 DirCache=1 HSDir=1 HSIntro=3 HSRend=1 \
                 Link=1-4 LinkAuth=1 Microdesc=1 Relay=1-2\0";
    }

    NUL_BYTE
}

#[cfg(test)]
mod test {
    use std::str::FromStr;
    use std::string::ToString;

    use super::*;

    #[test]
    fn test_versions_from_version_string() {
        use std::collections::HashSet;

        use super::Versions;

        assert_eq!(Err("invalid protocol entry"), Versions::from_version_string("a,b"));
        assert_eq!(Err("invalid protocol entry"), Versions::from_version_string("1,!"));

        {
            let mut versions: HashSet<Version> = HashSet::new();
            versions.insert(1);
            assert_eq!(versions, Versions::from_version_string("1").unwrap().0);
        }
        {
            let mut versions: HashSet<Version> = HashSet::new();
            versions.insert(1);
            versions.insert(2);
            assert_eq!(versions, Versions::from_version_string("1,2").unwrap().0);
        }
        {
            let mut versions: HashSet<Version> = HashSet::new();
            versions.insert(1);
            versions.insert(2);
            versions.insert(3);
            assert_eq!(versions, Versions::from_version_string("1-3").unwrap().0);
        }
        {
            let mut versions: HashSet<Version> = HashSet::new();
            versions.insert(1);
            versions.insert(2);
            versions.insert(5);
            assert_eq!(versions, Versions::from_version_string("1-2,5").unwrap().0);
        }
        {
            let mut versions: HashSet<Version> = HashSet::new();
            versions.insert(1);
            versions.insert(3);
            versions.insert(4);
            versions.insert(5);
            assert_eq!(versions, Versions::from_version_string("1,3-5").unwrap().0);
        }
    }

    #[test]
    fn test_contains_only_supported_protocols() {
        use super::contains_only_supported_protocols;

        assert_eq!(false, contains_only_supported_protocols(""));
        assert_eq!(true, contains_only_supported_protocols("Cons="));
        assert_eq!(true, contains_only_supported_protocols("Cons=1"));
        assert_eq!(false, contains_only_supported_protocols("Cons=0"));
        assert_eq!(false, contains_only_supported_protocols("Cons=0-1"));
        assert_eq!(false, contains_only_supported_protocols("Cons=5"));
        assert_eq!(false, contains_only_supported_protocols("Cons=1-5"));
        assert_eq!(false, contains_only_supported_protocols("Cons=1,5"));
        assert_eq!(false, contains_only_supported_protocols("Cons=5,6"));
        assert_eq!(false, contains_only_supported_protocols("Cons=1,5,6"));
        assert_eq!(true, contains_only_supported_protocols("Cons=1,2"));
        assert_eq!(true, contains_only_supported_protocols("Cons=1-2"));
    }

    #[test]
    fn test_find_range() {
        use super::find_range;

        assert_eq!((false, 0), find_range(&vec![]));
        assert_eq!((false, 1), find_range(&vec![1]));
        assert_eq!((true, 2), find_range(&vec![1, 2]));
        assert_eq!((true, 3), find_range(&vec![1, 2, 3]));
        assert_eq!((true, 3), find_range(&vec![1, 2, 3, 5]));
    }

    #[test]
    fn test_expand_version_range() {
        use super::expand_version_range;

        assert_eq!(Err("version string empty"), expand_version_range(""));
        assert_eq!(Ok(1..3), expand_version_range("1-2"));
        assert_eq!(Ok(1..5), expand_version_range("1-4"));
        assert_eq!(
            Err("cannot parse protocol range lower bound"),
            expand_version_range("a")
        );
        assert_eq!(
            Err("cannot parse protocol range upper bound"),
            expand_version_range("1-a")
        );
        assert_eq!(Ok(1000..66536), expand_version_range("1000-66535"));
        assert_eq!(Err("Too many protocols in expanded range"),
                   expand_version_range("1000-66536"));
    }

    #[test]
    fn test_contract_protocol_list() {
        use std::collections::HashSet;
        use super::contract_protocol_list;

        {
            let mut versions = HashSet::<Version>::new();
            assert_eq!(String::from(""), contract_protocol_list(&versions));

            versions.insert(1);
            assert_eq!(String::from("1"), contract_protocol_list(&versions));

            versions.insert(2);
            assert_eq!(String::from("1-2"), contract_protocol_list(&versions));
        }

        {
            let mut versions = HashSet::<Version>::new();
            versions.insert(1);
            versions.insert(3);
            assert_eq!(String::from("1,3"), contract_protocol_list(&versions));
        }

        {
            let mut versions = HashSet::<Version>::new();
            versions.insert(1);
            versions.insert(2);
            versions.insert(3);
            versions.insert(4);
            assert_eq!(String::from("1-4"), contract_protocol_list(&versions));
        }

        {
            let mut versions = HashSet::<Version>::new();
            versions.insert(1);
            versions.insert(3);
            versions.insert(5);
            versions.insert(6);
            versions.insert(7);
            assert_eq!(
                String::from("1,3,5-7"),
                contract_protocol_list(&versions)
            );
        }

        {
            let mut versions = HashSet::<Version>::new();
            versions.insert(1);
            versions.insert(2);
            versions.insert(3);
            versions.insert(500);
            assert_eq!(
                String::from("1-3,500"),
                contract_protocol_list(&versions)
            );
        }
    }
}
