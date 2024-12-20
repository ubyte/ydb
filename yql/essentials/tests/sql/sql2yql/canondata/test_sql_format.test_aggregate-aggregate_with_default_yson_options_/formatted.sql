USE plato;

SELECT
    key,
    Yson::SerializeJson(Yson::From(AGGREGATE_LIST(value))) AS value
FROM
    Input
GROUP BY
    key
ORDER BY
    key
;
