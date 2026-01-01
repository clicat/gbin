mod codec;
mod error;
mod header;
mod value;

pub use crate::codec::{read_file, read_header_only, read_var, write_file, CompressionMode, ReadOptions, WriteOptions};
pub use crate::error::{GbfError, Result};
pub use crate::header::{FieldMeta, Header, MAGIC_BYTES, VERSION};
pub use crate::value::{
    element_count, CalendarDurationArray, CategoricalArray, CharArray, DateTimeArray, DurationArray,
    GbfValue, LogicalArray, NumericArray, NumericClass, StringArray,
};