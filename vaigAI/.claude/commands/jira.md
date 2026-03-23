# JIRA Ticket Lookup

Fetch JIRA ticket details from Citrix Atlassian and extract the linked SR number.

## Input

The user provides a JIRA ticket key as `$ARGUMENTS` (e.g., `NSPLAT-12345`, `NSCXLCM-67890`).

Recognized JIRA project keys: `NSPLAT`, `NSCXLCM`, `NSHELP`, `NSSVM`, `NSNET`, `NSGSLB`, `NSSSL`.

## Credentials

- **Email**: `shankar.krishnamurthy@citrix.com`
- **API Token**: `YOUR_ATLASSIAN_API_TOKEN_HERE`

## Workflow

### 1. Fetch JIRA Details

Run this command (store in shell variable to avoid file writes):

```bash
JIRA_DATA=$(curl -s -u shankar.krishnamurthy@citrix.com:YOUR_ATLASSIAN_API_TOKEN_HERE \
  -H "Accept: application/json" \
  "https://citrix.atlassian.net/rest/api/2/issue/$ARGUMENTS?expand=comments")
```

### 2. Extract Key Information

Parse the JSON response directly from the shell variable:

```bash
# Extract basic JIRA info
echo "$JIRA_DATA" | jq -r '{
  key: .key,
  summary: .fields.summary,
  status: .fields.status.name,
  priority: .fields.priority.name,
  assignee: .fields.assignee.displayName,
  reporter: .fields.reporter.displayName,
  resolution: .fields.resolution.name,
  created: .fields.created,
  updated: .fields.updated,
  description: .fields.description,
  comment_count: .fields.comment.total
}'

# Search for SR number (9-digit pattern) across entire response
echo "$JIRA_DATA" | grep -oE '[0-9]{9}' | head -1

# Extract recent comments
echo "$JIRA_DATA" | jq -r '.fields.comment.comments[] | "[\(.created)] \(.author.displayName):\n\(.body)\n---"' | tail -50
```

**SR Number Search Locations** (in priority order):
1. **Summary field**: Look for `\d{9}` pattern (9-digit SR number)
2. **Description**: Search for SR number in issue description
3. **Comments**: Search all comments for SR references
4. **Custom fields**: Check for custom SR number fields
5. **Links**: Check for linked issues that might contain SR

### 3. Generate JIRA Summary

Create a ~500-word summary with these sections:

1. **Issue Overview**: Title/summary, current status and priority
2. **Description**: Problem statement, affected components/versions
3. **Customer Impact**: Severity/priority justification, business impact
4. **Technical Details**: Error messages/symptoms, affected features, environment details
5. **Activity Summary**: Last 3-5 relevant comments, status changes, assigned team
6. **Related Issues**: Linked JIRAs or SRs

### 4. Decision Point

**If SR Number Found**: Present the JIRA summary with the linked SR number and note "Proceeding with SR analysis..." — then route to the debug-agent workflow with the extracted SR number.

**If NO SR Number Found**: Present the JIRA summary and note that no SR number was found. Recommend:
- Check if SR exists but is not mentioned in JIRA
- Request customer to provide SR number
- Analyze JIRA content for resolution without bundle analysis

Stop workflow — do not proceed to bundle analysis.

## Output Format

```markdown
## JIRA Analysis: [ISSUE-KEY]

**Summary**: [Issue title]
**Status**: [Current status] | **Priority**: [Priority level]
**Assignee**: [Assigned to] | **Reporter**: [Reported by]
**Created**: [Date] | **Updated**: [Last update]

### Issue Description
[Description text - formatted and readable]

### Problem Statement
[Customer's reported issue - extracted key points]

### Environment
- **Product**: [NetScaler/SDX/other]
- **Version**: [Product version if mentioned]
- **Platform**: [VPX/MPX/SDX/CPX if mentioned]

### Activity Summary
[Last 3-5 relevant comments or updates]

### Linked SR
**SR Number**: SR[number] (if found)
**SR Status**: [If available from comments]

---

**Next Steps**:
[Either "Proceeding with SR bundle analysis..." or "No SR found - manual investigation required"]
```

## Error Handling

- **401 Unauthorized**: Verify API token is valid, check email format, ensure no extra spaces in credentials
- **404 Not Found**: Verify JIRA key format (PROJECT-NUMBER), check if issue exists, verify user access
- **429 Too Many Requests**: Wait and retry after delay
