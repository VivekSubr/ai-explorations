# REST apis in C++ 

**openapi-generator-cli** is a command-line tool that generates client SDKs, server stubs, API documentation, and configuration files from OpenAPI Specification.

It generates *models* and *apis* from the yaml.

**Models** are data-structures, in out case yaml objects become C++ class,

eg:
```
WrappedJsonValue:
  type: object
  required: [data]
  properties:
    data:
      $ref: '#/components/schemas/JsonValue'
    expiry_sec:
      type: integer
      format: int64
      minimum: 1
      description: Time-to-live for the entry, in seconds
```

Becomes 
```
// WrappedJsonValue.h
namespace org::openapitools::client::model {

class WrappedJsonValue {
public:
    WrappedJsonValue();
    virtual ~WrappedJsonValue();
    
    // Required field: data
    web::json::value getData() const;
    void setData(const web::json::value& value);
    bool dataIsSet() const;
    
    // Optional field: expiry_sec
    int64_t getExpirySec() const;
    void setExpirySec(int64_t value);
    bool expirySecIsSet() const;
    void unsetExpirySec();
    
    // Serialization
    web::json::value toJson() const;
    void fromJson(const web::json::value& json);
    bool validate();

protected:
    web::json::value m_Data;
    bool m_DataIsSet;
    
    int64_t m_ExpirySec;
    bool m_ExpirySecIsSet;
};

}```
 

**Apis** are C++ public functions generated from Yaml Paths. Eg:
```
/{key}:
  parameters:
    - name: key
      in: path
      required: true
      schema:
        type: string
  get:
    tags: [kv]
    summary: Get the value for a key
    responses:
      '200':
        description: Value for the given key
        content:
          application/json:
            schema:
              $ref: '#/components/schemas/JsonValue'
      '404':
        $ref: '#/components/responses/NotFound'
```

Becomes 
```
// KvApi.h
namespace org::openapitools::client::api {

class KvApi {
public:
    KvApi(std::shared_ptr<ApiClient> apiClient);
    
    /// <summary>
    /// Get the value for a key
    /// </summary>
    /// <param name="key">Cache key (required)</param>
    pplx::task<std::shared_ptr<web::json::value>> keyGet(
        const utility::string_t& key
    );

private:
    std::shared_ptr<ApiClient> m_ApiClient;
};

}

// KvApi.cpp
pplx::task<std::shared_ptr<web::json::value>> 
KvApi::keyGet(const utility::string_t& key) {
    
    // Validate required parameter
    if (key.empty()) {
        throw ApiException(400, 
            utility::conversions::to_string_t("Missing required parameter 'key'"));
    }
    
    // Build request path: /{key}
    utility::string_t path = utility::conversions::to_string_t("/{key}");
    boost::replace_all(path, 
        utility::conversions::to_string_t("{key}"), 
        m_ApiClient->parameterToString(key));
    
    // Setup HTTP request
    std::map<utility::string_t, utility::string_t> queryParams;
    std::map<utility::string_t, utility::string_t> headerParams;
    
    std::vector<utility::string_t> accepts = {
        utility::conversions::to_string_t("application/json")
    };
    
    // Make async HTTP call
    return m_ApiClient->callApi(
        path,
        U("GET"),
        queryParams,
        web::json::value::null(),
        headerParams,
        accepts,
        utility::conversions::to_string_t("application/json")
    ).then([](web::http::http_response response) {
        if (response.status_code() == 200) {
            return response.extract_json();
        }
        // Handle errors (404, etc.)
        throw ApiException(response.status_code(), 
            utility::conversions::to_string_t("Error calling keyGet"));
    }).then([](web::json::value json) {
        return std::make_shared<web::json::value>(json);
    });
}```


## What rest.cc actually uses
