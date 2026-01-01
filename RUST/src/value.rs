use std::collections::BTreeMap;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum NumericClass {
    Double,
    Single,
    Int8,
    Uint8,
    Int16,
    Uint16,
    Int32,
    Uint32,
    Int64,
    Uint64,
}

impl NumericClass {
    pub fn from_matlab_class(s: &str) -> Option<Self> {
        match s.to_ascii_lowercase().as_str() {
            "double" => Some(Self::Double),
            "single" => Some(Self::Single),
            "int8" => Some(Self::Int8),
            "uint8" => Some(Self::Uint8),
            "int16" => Some(Self::Int16),
            "uint16" => Some(Self::Uint16),
            "int32" => Some(Self::Int32),
            "uint32" => Some(Self::Uint32),
            "int64" => Some(Self::Int64),
            "uint64" => Some(Self::Uint64),
            _ => None,
        }
    }

    pub fn as_matlab_class(&self) -> &'static str {
        match self {
            Self::Double => "double",
            Self::Single => "single",
            Self::Int8 => "int8",
            Self::Uint8 => "uint8",
            Self::Int16 => "int16",
            Self::Uint16 => "uint16",
            Self::Int32 => "int32",
            Self::Uint32 => "uint32",
            Self::Int64 => "int64",
            Self::Uint64 => "uint64",
        }
    }

    pub fn bytes_per_element(&self) -> usize {
        match self {
            Self::Double => 8,
            Self::Single => 4,
            Self::Int8 => 1,
            Self::Uint8 => 1,
            Self::Int16 => 2,
            Self::Uint16 => 2,
            Self::Int32 => 4,
            Self::Uint32 => 4,
            Self::Int64 => 8,
            Self::Uint64 => 8,
        }
    }
}

pub fn element_count(shape: &[usize]) -> usize {
    shape.iter().copied().fold(1usize, |acc, d| acc.saturating_mul(d))
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct NumericArray {
    pub class: NumericClass,
    pub shape: Vec<usize>,
    pub complex: bool,
    pub real_le: Vec<u8>,
    pub imag_le: Option<Vec<u8>>,
}

impl NumericArray {
    pub fn bytes_per_part(&self) -> usize {
        element_count(&self.shape) * self.class.bytes_per_element()
    }

    pub fn new_real(class: NumericClass, shape: Vec<usize>, real_le: Vec<u8>) -> Self {
        Self {
            class,
            shape,
            complex: false,
            real_le,
            imag_le: None,
        }
    }

    pub fn new_complex(
        class: NumericClass,
        shape: Vec<usize>,
        real_le: Vec<u8>,
        imag_le: Vec<u8>,
    ) -> Self {
        Self {
            class,
            shape,
            complex: true,
            real_le,
            imag_le: Some(imag_le),
        }
    }

    pub fn from_f64_column_major(shape: Vec<usize>, data: Vec<f64>) -> Self {
        let mut bytes = Vec::with_capacity(data.len() * 8);
        for v in data {
            bytes.extend_from_slice(&v.to_le_bytes());
        }
        Self::new_real(NumericClass::Double, shape, bytes)
    }

    pub fn from_f32_column_major(shape: Vec<usize>, data: Vec<f32>) -> Self {
        let mut bytes = Vec::with_capacity(data.len() * 4);
        for v in data {
            bytes.extend_from_slice(&v.to_le_bytes());
        }
        Self::new_real(NumericClass::Single, shape, bytes)
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LogicalArray {
    pub shape: Vec<usize>,
    /// One byte per element (0/1), column-major order.
    pub data: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CharArray {
    pub shape: Vec<usize>,
    /// UTF-16 code units, column-major order.
    pub data: Vec<u16>,
}

impl CharArray {
    /// Convenience for a 1xN MATLAB char row (UTF-16 code units).
    pub fn from_str_row(s: &str) -> Self {
        let data: Vec<u16> = s.encode_utf16().collect();
        Self {
            shape: vec![1, data.len()],
            data,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct StringArray {
    pub shape: Vec<usize>,
    /// Flattened column-major order.
    pub data: Vec<Option<String>>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DateTimeArray {
    pub shape: Vec<usize>,
    pub tz: Option<String>,
    pub locale: Option<String>,
    pub format: Option<String>,

    /// 0/1 mask, length = N
    pub is_nat: Vec<u8>,

    /// Components, length = N (0 when NaT)
    pub year: Vec<i16>,
    pub month: Vec<u8>,
    pub day: Vec<u8>,
    pub ms_day: Vec<i32>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DurationArray {
    pub shape: Vec<usize>,
    /// 0/1 mask, length = N
    pub is_nan: Vec<u8>,
    /// milliseconds, length = N (0 where NaN)
    pub ms: Vec<i64>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CalendarDurationArray {
    pub shape: Vec<usize>,
    /// 0/1 mask, length = N
    pub is_missing: Vec<u8>,
    pub months: Vec<i32>,
    pub days: Vec<i32>,
    pub time_ms: Vec<i64>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CategoricalArray {
    pub shape: Vec<usize>,
    pub categories: Vec<String>,
    /// 0 for <undefined>, else 1..=categories.len()
    pub codes: Vec<u32>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum GbfValue {
    Struct(BTreeMap<String, GbfValue>),

    Numeric(NumericArray),
    Logical(LogicalArray),
    Char(CharArray),
    String(StringArray),
    DateTime(DateTimeArray),
    Duration(DurationArray),
    CalendarDuration(CalendarDurationArray),
    Categorical(CategoricalArray),

    /// Represents MATLAB `struct()` with no fields (scalar empty struct)
    EmptyStruct,
}

impl GbfValue {
    pub fn as_struct(&self) -> Option<&BTreeMap<String, GbfValue>> {
        match self {
            GbfValue::Struct(m) => Some(m),
            _ => None,
        }
    }

    pub fn get_path(&self, path: &str) -> Option<&GbfValue> {
        if path.is_empty() {
            return Some(self);
        }
        let mut cur = self;
        for part in path.split('.') {
            match cur {
                GbfValue::Struct(m) => {
                    cur = m.get(part)?;
                }
                _ => return None,
            }
        }
        Some(cur)
    }
}