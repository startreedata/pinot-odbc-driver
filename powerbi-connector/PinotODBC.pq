// Apache Pinot — Power BI custom connector (DirectQuery + Import).
//
// Wraps the StarTree Pinot ODBC driver with Odbc.DataSource so Power BI can
// fold queries down to Pinot SQL (DirectQuery) instead of importing whole
// tables. Adapted from Microsoft's SqlODBC sample
// (https://github.com/microsoft/DataConnectors, MIT).
//
// Copyright 2026 StarTree Inc. Licensed under the StarTree Community License;
// see ../LICENSE.
[Version = "1.0.0"]
section PinotODBC;

// Trace output to the Power Query user log. Keep false for release builds.
EnableTraceOutput = false;

/****************************
 * ODBC Driver Configuration
 ****************************/

// Must match the driver name registered with the Windows ODBC driver manager
// (see examples/install_windows.ps1).
Config_DriverName = "StarTree Pinot ODBC Driver";

// Pinot's SELECT dialect is reported by the driver as SQL-92 entry level.
// Bump this only if you find Power BI under-folding and your cluster handles
// the richer SQL it then generates.
Config_SqlConformance = ODBC[SQL_SC][SQL_SC_SQL92_ENTRY];
// null, 1, 2, 4, 8

// Pinot uses "... LIMIT n" (no OFFSET keyword), so LimitClauseKind.Limit.
Config_LimitClauseKind = LimitClauseKind.Limit;

// Credentials are handled explicitly below (Anonymous / Key / UsernamePassword),
// so the automatic UID/PWD handling is disabled.
Config_DefaultUsernamePasswordHandling = false;

// The driver substitutes parameters as escaped SQL literals (Pinot's HTTP API
// has no server-side prepare), so inline literals rather than bind parameters.
Config_UseParameterBindings = false;

// Pinot escapes a single quote by doubling it.
Config_StringLiteralEscapeCharacters = {"'"};

// Pinot uses CAST(x AS type), not the ODBC CONVERT() scalar.
Config_UseCastInsteadOfConvert = true;

// Enable DirectQuery in addition to Import mode.
Config_EnableDirectQuery = true;

[DataSource.Kind = "PinotODBC", Publish = "PinotODBC.Publish"]
shared PinotODBC.Database = (host as text, port as number, optional options as record) as table =>
    let
        // Optional connection settings surfaced through the options record.
        ValidOptionsMap = #table(
            {"Name", "Type", "Description", "Default", "Validate"},
            {
                {
                    "Scheme",
                    type nullable text,
                    "http or https",
                    "http",
                    each _ = null or _ = "http" or _ = "https"
                },
                {
                    "Controller",
                    type nullable text,
                    "controller host:port used for table/schema discovery",
                    null,
                    each _ = null or _ <> null
                },
                {
                    "Database",
                    type nullable text,
                    "Pinot database name (sent as the database header)",
                    null,
                    each _ = null or _ <> null
                },
                {
                    "UseMultistage",
                    type nullable logical,
                    "use the multi-stage query engine (needed for JOINs)",
                    null,
                    each _ = null or _ = true or _ = false
                },
                {
                    "ConnectionTimeout",
                    type nullable number,
                    "HTTP timeout in seconds",
                    null,
                    each _ = null or (_ >= 0 and Number.RoundDown(_) = _)
                }
            }
        ),
        ValidatedOptions = ValidateOptions(options, ValidOptionsMap),
        //
        // Connection string, built as explicit "KEY=value;..." text. Required
        // keys (Driver/Host/Port) are always present; optional keys are added
        // only when set.
        //
        ConnectionString = Text.Combine(
            List.Combine({
                {
                    "Driver={" & Config_DriverName & "}",
                    "Host=" & host,
                    "Port=" & Text.From(port)
                },
                if ValidatedOptions[Scheme]? <> null then {"Scheme=" & ValidatedOptions[Scheme]} else {},
                if ValidatedOptions[Controller]? <> null then {"Controller=" & ValidatedOptions[Controller]} else {},
                if ValidatedOptions[Database]? <> null then {"Database=" & ValidatedOptions[Database]} else {},
                if ValidatedOptions[UseMultistage]? = true then {"UseMultistage=1"} else {},
                if ValidatedOptions[ConnectionTimeout]? <> null then
                    {"Timeout=" & Text.From(ValidatedOptions[ConnectionTimeout])}
                else
                    {}
            }),
            ";"
        ),
        //
        // Credentials are set separately from the persisted connection string.
        // Anonymous adds nothing (the driver talks to the cluster without auth).
        //
        Credential = Extension.CurrentCredential(),
        CredentialConnectionString =
            if Credential[AuthenticationKind]? = "UsernamePassword" then
                [UID = Credential[Username], PWD = Credential[Password]]
            else if Credential[AuthenticationKind]? = "Key" then
                // Bearer token -> the driver prepends "Bearer " automatically.
                [Token = Credential[Key]]
            else
                [],
        //
        // Capabilities derived from the configuration variables above.
        //
        defaultConfig = BuildOdbcConfig(),
        SqlCapabilities = defaultConfig[SqlCapabilities],
        SQLGetFunctions = defaultConfig[SQLGetFunctions],
        SQLGetInfo = defaultConfig[SQLGetInfo],
        // Base options; only attach CredentialConnectionString when it has
        // properties (an empty one is rejected as "no properties defined").
        BaseOptions = [
            // Pinot has a flat table namespace (no schemas/catalogs).
            HierarchicalNavigation = false,
            SoftNumbers = true,
            TolerateConcatOverflow = true,
            ClientConnectionPooling = true,
            SqlCapabilities = SqlCapabilities,
            SQLGetFunctions = SQLGetFunctions,
            SQLGetInfo = SQLGetInfo
        ],
        OdbcOptions =
            if Record.FieldCount(CredentialConnectionString) > 0 then
                BaseOptions & [CredentialConnectionString = CredentialConnectionString]
            else
                BaseOptions,
        OdbcDatasource = Odbc.DataSource(ConnectionString, OdbcOptions)
    in
        OdbcDatasource;

// Data Source Kind description.
PinotODBC = [
    // Enables gateway support: validates credentials by invoking the function.
    TestConnection = (dataSourcePath) =>
        let
            json = Json.Document(dataSourcePath),
            host = json[host],
            port = json[port]
        in
            {"PinotODBC.Database", host, port},
    Authentication = [
        // No auth (open clusters / quickstart).
        Anonymous = [],
        // Bearer token (StarTree Cloud and secured clusters).
        Key = [
            KeyLabel = "API token"
        ],
        // HTTP Basic.
        UsernamePassword = []
    ],
    Label = "Apache Pinot"
];

// Data Source UI publishing description.
PinotODBC.Publish = [
    Beta = true,
    Category = "Database",
    ButtonText = {"Apache Pinot", "Connect to an Apache Pinot cluster"},
    LearnMoreUrl = "https://github.com/startreedata/pinot-odbc-driver",
    SupportsDirectQuery = Config_EnableDirectQuery,
    SourceImage = PinotODBC.Icons,
    SourceTypeImage = PinotODBC.Icons
];

PinotODBC.Icons = [
    Icon16 = {
        Extension.Contents("PinotODBC16.png"),
        Extension.Contents("PinotODBC20.png"),
        Extension.Contents("PinotODBC24.png"),
        Extension.Contents("PinotODBC32.png")
    },
    Icon32 = {
        Extension.Contents("PinotODBC32.png"),
        Extension.Contents("PinotODBC40.png"),
        Extension.Contents("PinotODBC48.png"),
        Extension.Contents("PinotODBC64.png")
    }
];

// Build SqlCapabilities / SQLGetFunctions / SQLGetInfo records from the
// configuration variables above. (From Microsoft's SqlODBC sample.)
BuildOdbcConfig = () as record =>
    let
        Merge = (previous as record, optional caps as record, optional funcs as record, optional getInfo as record) as record =>
            let
                newCaps = if (caps <> null) then previous[SqlCapabilities] & caps else previous[SqlCapabilities],
                newFuncs = if (funcs <> null) then previous[SQLGetFunctions] & funcs else previous[SQLGetFunctions],
                newGetInfo = if (getInfo <> null) then previous[SQLGetInfo] & getInfo else previous[SQLGetInfo]
            in
                [SqlCapabilities = newCaps, SQLGetFunctions = newFuncs, SQLGetInfo = newGetInfo],
        defaultConfig = [
            SqlCapabilities = [],
            SQLGetFunctions = [],
            SQLGetInfo = []
        ],
        withParams =
            if (Config_UseParameterBindings = false) then
                let
                    caps = [
                        SupportsNumericLiterals = true,
                        SupportsStringLiterals = true,
                        SupportsOdbcDateLiterals = true,
                        SupportsOdbcTimeLiterals = true,
                        SupportsOdbcTimestampLiterals = true
                    ],
                    funcs = [
                        SQL_API_SQLBINDPARAMETER = false
                    ]
                in
                    Merge(defaultConfig, caps, funcs)
            else
                defaultConfig,
        withEscape =
            if (Config_StringLiteralEscapeCharacters <> null) then
                let
                    caps = [
                        StringLiteralEscapeCharacters = Config_StringLiteralEscapeCharacters
                    ]
                in
                    Merge(withParams, caps)
            else
                withParams,
        withLimitClauseKind = let caps = [
            LimitClauseKind = Config_LimitClauseKind
        ] in Merge(withEscape, caps),
        withCastOrConvert =
            if (Config_UseCastInsteadOfConvert <> null) then
                let
                    value =
                        if (Config_UseCastInsteadOfConvert = true) then
                            ODBC[SQL_FN_CVT][SQL_FN_CVT_CAST]
                        else
                            ODBC[SQL_FN_CVT][SQL_FN_CVT_CONVERT],
                    getInfo = [
                        SQL_CONVERT_FUNCTIONS = value
                    ]
                in
                    Merge(withLimitClauseKind, null, null, getInfo)
            else
                withLimitClauseKind,
        withSqlConformance =
            if (Config_SqlConformance <> null) then
                let
                    getInfo = [
                        SQL_SQL_CONFORMANCE = Config_SqlConformance
                    ]
                in
                    Merge(withCastOrConvert, null, null, getInfo)
            else
                withCastOrConvert
    in
        withSqlConformance;

// Validates the optional options record against ValidOptionsMap.
// (From Microsoft's SqlODBC sample.)
ValidateOptions = (options as nullable record, validOptionsMap as table) as record =>
    let
        ValidKeys = Table.Column(validOptionsMap, "Name"),
        InvalidKeys = List.Difference(Record.FieldNames(options), ValidKeys),
        InvalidKeysText =
            if List.IsEmpty(InvalidKeys) then
                null
            else
                Text.Format(
                    "'#{0}' are not valid options. Valid options are: '#{1}'",
                    {Text.Combine(InvalidKeys, ", "), Text.Combine(ValidKeys, ", ")}
                ),
        ValidateValue = (name, optionType, description, default, validate, value) =>
            if
                (value is null and (Type.IsNullable(optionType) or default <> null))
                or (Type.Is(Value.Type(value), optionType) and validate(value))
            then
                null
            else
                Text.Format(
                    "This function does not support the option '#{0}' with value '#{1}'. Valid value is #{2}.",
                    {name, value, description}
                ),
        InvalidValues = List.RemoveNulls(
            Table.TransformRows(
                validOptionsMap,
                each
                    ValidateValue(
                        [Name],
                        [Type],
                        [Description],
                        [Default],
                        [Validate],
                        Record.FieldOrDefault(options, [Name], [Default])
                    )
            )
        ),
        DefaultOptions = Record.FromTable(
            Table.RenameColumns(Table.SelectColumns(validOptionsMap, {"Name", "Default"}), {"Default", "Value"})
        ),
        NullNotAllowedFields = List.RemoveNulls(
            Table.TransformRows(
                validOptionsMap,
                each
                    if not Type.IsNullable([Type]) and null = Record.FieldOrDefault(options, [Name], [Default]) then
                        [Name]
                    else
                        null
            )
        ),
        NormalizedOptions = DefaultOptions & Record.RemoveFields(options, NullNotAllowedFields, MissingField.Ignore)
    in
        if null = options then
            DefaultOptions
        else if not List.IsEmpty(InvalidKeys) then
            error Error.Record("Expression.Error", InvalidKeysText)
        else if not List.IsEmpty(InvalidValues) then
            error Error.Record("Expression.Error", Text.Combine(InvalidValues, ", "))
        else
            NormalizedOptions;

// Load common library functions vendored from Microsoft's DataConnectors.
Extension.LoadFunction = (name as text) =>
    let
        binary = Extension.Contents(name),
        asText = Text.FromBinary(binary)
    in
        Expression.Evaluate(asText, #shared);

// OdbcConstants provides the ODBC numeric constant tables (SQL_SC, SQL_FN_CVT, ...).
ODBC = Extension.LoadFunction("OdbcConstants.pqm");
