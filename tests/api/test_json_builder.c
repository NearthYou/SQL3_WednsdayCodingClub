#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/api/db/db_wrapper.h"
#include "../../src/api/json/json_builder.h"

int main(void) {
    DbResult result;
    DbRow row;
    DbCell cells[2];
    char *json = NULL;
    size_t len = 0;

    memset(&result, 0, sizeof(result));
    memset(&row, 0, sizeof(row));
    memset(cells, 0, sizeof(cells));

    cells[0].name = "id";
    cells[0].value = "1";
    cells[1].name = "email";
    cells[1].value = "admin@test.com";
    row.cell_count = 2;
    row.cells = cells;

    result.ok = 1;
    result.http_status = 200;
    result.row_count = 1;
    result.rows = &row;

    assert(json_build_response(&result, &json, &len) == 1);
    assert(strstr(json, "\"status\":\"ok\"") != NULL);
    assert(strstr(json, "\"email\":\"admin@test.com\"") != NULL);
    free(json);

    result.ok = 0;
    result.error_message = "invalid SQL";
    assert(json_build_response(&result, &json, &len) == 1);
    assert(strstr(json, "\"status\":\"error\"") != NULL);
    assert(strstr(json, "invalid SQL") != NULL);
    free(json);

    printf("test_json_builder: OK\n");
    return 0;
}
